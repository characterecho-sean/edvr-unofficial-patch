// vertex_resync_test: the game's stale vertex-buffer binding and the repair at the entry of Frontier's input-assembler flush (src/d3d11/vertex_resync_core.h,
// vertex_resync_hook.cpp; docs/scanner-body.md, "The root cause: Frontier's f3d state cache").
//
//   --dry-run    the same run except the one check that writes a log file in the temp folder (V5.m-p), for the gate's --dry-run convention
//   --self-test  every check; the optional argument is the repository root, where the glue is read as text (the mutation tool hands it a temp root holding an edited copy)
//
// The pure half runs on a fake command list laid out at the real offsets (a model of SetVertexBuffer 0x1404E9D70 and of FlushIA's slot loop 0x140522A50, written from the
// disassembly in analysis\decomp\scanner_vb_dump_*). The hook half is the production hook.cpp compiled with EDVR_VERTEX_RESYNC_TEST: the real CodeHook patches a synthetic
// function that begins with the real 16-byte prologue of FlushIA, the real relay runs, the real SEH-guarded repair runs before the original. The repair has no key: there is
// no off to test. Cases ("V<case>.<what>"; mutants.py names the case that must catch each mutation):
//   V1  the failing sequence replayed (resolve binds Q; two zero-slot draws zero the applied slot; the second eye's SetVertexBuffer(Q) is skipped): stock binds (NULL, 20, 0);
//       repaired it binds Q with the desired offset; slots at or above the layout's count, a null desired slot, a count above 16, a null pointer anywhere, and a healthy list
//       are left alone
//   V2  the gate and the prologue: the PE pair, the 16 bytes (every position), the length
//   V3  the instruments: the exact lines (60 s, heartbeat, session, first sighting), the windows (60 s and 10 min, zero counts included), the flush counter, the sighting cap
//   V4  the hook end to end on a synthetic FlushIA: a wrong prologue is refused with a line and nothing patched, and nothing armed says nothing at all; the right one arms
//       (stolen = 5); the original sees the repaired cache; flushes are counted; the sightings are capped
//   V5  what the log says while armed: the 60 s count, the ten-minute heartbeat with zero counts and per-window deltas, the session line at shutdown, the process-exit line
//       through the real Log
//   V6  a fault is absorbed and eight stand the hook down; a stood-down hook says no heartbeat and no session line, and counts no more flushes
//   V7  the glue, read as text: device_hook installs once after the other startup installers, polls once a second and says the session line at shutdown; the hook registers
//       its exit line with the log and reads no key; the flush counter is a load and a store, never a read-modify-write
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "config.h"
#include "log.h"
#include "vertex_resync_core.h"
#include "vertex_resync_hook.h"

namespace edvr {
void breadcrumb(const char*) {}   // production guard.cpp's crash-channel dependency (proxy.cpp), as the other rigs stub it
}  // namespace edvr

namespace vr = edvr::vresync;

// The case ids the checks label themselves with (tools\vertex_resync_test\mutants.py reads the labels).
static const char* const kCases[] = {"V1.replay", "V2.gate", "V3.instruments", "V4.hook", "V5.report", "V6.faults", "V7.glue"};

namespace {
unsigned g_checks = 0, g_failures = 0;
bool g_writesFiles = true;   // false under --dry-run: the one check that writes a log file in the temp folder is skipped
void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", label);
        std::fflush(stdout);
    }
}
bool has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }
size_t occurrences(const std::string& text, const char* needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}
// Both found, `first` before `second`.
bool precedes(const std::string& text, const char* first, const char* second) {
    const size_t a = text.find(first), b = text.find(second);
    return a != std::string::npos && b != std::string::npos && a < b;
}
bool readText(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// ---- a fake f3d command list, laid out at the real offsets ------------------------------------------------------------------------------------------------
struct Wrapper { alignas(16) uint8_t bytes[0x200]; };   // an f3d buffer object: the native ID3D11Buffer* at +0x140
constexpr uint64_t kNativeQ = 0x1000000010ull, kNativeR = 0x1000000020ull, kNativeS = 0x1000000030ull;   // never dereferenced

struct Bound { uint64_t buffer = 0; uint32_t stride = 0, offset = 0; };

struct FakeList {
    alignas(16) uint8_t list[0x600];
    alignas(16) uint8_t layout[0x100];
    alignas(16) uint8_t pso[0x200];
    alignas(16) uint8_t slotDesc[16][0x40];
    Wrapper wrappers[20];
    std::vector<Bound> lastBind;   // what the last flush bound, slot by slot

    FakeList() { std::memset(this, 0, offsetof(FakeList, lastBind)); wire(); }
    void wire() {
        vr::store64(pso + vr::kPsoLayout, reinterpret_cast<uintptr_t>(layout));
        for (int i = 0; i < 16; ++i) vr::store64(pso + 0x100 + 8 * i, reinterpret_cast<uintptr_t>(slotDesc[i]));
        for (int i = 0; i < 20; ++i) std::memset(wrappers[i].bytes, 0, sizeof(wrappers[i].bytes));
        vr::store64(wrappers[0].bytes + vr::kWrapperNative, kNativeQ);
        vr::store64(wrappers[1].bytes + vr::kWrapperNative, kNativeR);
        vr::store64(wrappers[2].bytes + vr::kWrapperNative, kNativeS);
    }
    uint8_t* desired() { return list + vr::kListDesired; }
    uint8_t* applied() { return list + vr::kListApplied; }
    // The draw's layout: `count` slots, each 16-bit stride in its descriptor (FlushIA reads it at [slotDesc+0x38]).
    void setLayout(uint32_t count, uint16_t stride) {
        vr::store32(layout + vr::kLayoutSlots, count);
        for (int i = 0; i < 16; ++i) {
            slotDesc[i][0x38] = static_cast<uint8_t>(stride & 0xFF);
            slotDesc[i][0x39] = static_cast<uint8_t>(stride >> 8);
        }
    }
    uint8_t* wrapperPtr(int i) { return i < 0 ? nullptr : wrappers[i].bytes; }

    // SetVertexBuffer (0x1404E9D70): compares ONLY the desired state; on a change writes desired and applied together and sets dirty bit 0x20.
    void setVertexBuffer(uint32_t slot, uint8_t* wrapper, uint32_t offset, uint32_t over = 0xFFFFFFFFu) {
        const uint64_t w = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(wrapper));
        if (vr::load64(list + 0x60 + 8 * slot) == w && vr::load32(list + 0xE0 + 4 * slot) == offset && vr::load32(list + 0x120 + 4 * slot) == over) return;   // skipped
        vr::store64(list + 0x60 + 8 * slot, w);
        vr::store32(list + 0xE0 + 4 * slot, offset);
        vr::store32(list + 0x120 + 4 * slot, over);
        vr::store64(list + 0x1E8, vr::load64(list + 0x1E8) | 0x20);
        if (wrapper) {
            vr::store64(list + 0x408 + 8 * slot, vr::load64(wrapper + vr::kWrapperNative));
            vr::store32(list + 0x4C8 + 4 * slot, offset);
        } else {
            vr::store64(list + 0x408 + 8 * slot, 0);
            vr::store32(list + 0x4C8 + 4 * slot, 0);
        }
        vr::store32(list + 0x488 + 4 * slot, 0);
    }
    // FlushIA's vertex-buffer half (0x140522A50): strides from the desired state, the "fewer slots" zeroing of the applied arrays, then the bind from the applied arrays.
    // `dirty` is the word at list+0x1E8: with none of the mask's bits the function returns without binding.
    bool flush() {
        uint8_t* d = desired();
        uint8_t* a = applied();
        if ((vr::load64(list + 0x1E8) & 0xFBFE20u) == 0) return false;
        const uint32_t count = vr::load32(layout + vr::kLayoutSlots);
        for (uint32_t i = 0; i < count && i < 16; ++i) {
            if (!vr::load64(d + 8 * i)) continue;
            const uint64_t desc = vr::load64(pso + 0x100 + 8 * i);
            if (!desc) continue;
            const uint32_t over = vr::load32(d + 0xC0 + 4 * i);
            uint32_t stride = 0;
            if (over == 0xFFFFFFFFu) { uint16_t s; std::memcpy(&s, reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(desc)) + 0x38, 2); stride = s; }
            vr::store32(a + 0x1E8 + 4 * i, stride);
        }
        const uint32_t old = vr::load32(a + 0x118);
        for (uint32_t i = count; i < old && i < 16; ++i) {   // the fewer-slots loop: applied buffer, offset and stride go to zero; desired is never touched
            vr::store64(a + vr::kAppliedBuffers + 8 * i, 0);
            vr::store32(a + vr::kAppliedOffsets + 4 * i, 0);
            vr::store32(a + 0x1E8 + 4 * i, 0);
        }
        const uint32_t n = count > old ? count : old;
        lastBind.assign(n, Bound{});
        for (uint32_t i = 0; i < n && i < 16; ++i) {
            lastBind[i].buffer = vr::load64(a + vr::kAppliedBuffers + 8 * i);
            lastBind[i].stride = vr::load32(a + 0x1E8 + 4 * i);
            lastBind[i].offset = vr::load32(a + vr::kAppliedOffsets + 4 * i);
        }
        vr::store32(a + 0x118, count);
        return true;
    }
    void dirty(uint64_t bits) { vr::store64(list + 0x1E8, vr::load64(list + 0x1E8) | bits); }
    uint64_t appliedBuffer(uint32_t slot) { return vr::load64(applied() + vr::kAppliedBuffers + 8 * slot); }
    uint32_t appliedOffset(uint32_t slot) { return vr::load32(applied() + vr::kAppliedOffsets + 4 * slot); }
};

__declspec(noinline) bool tryResync(const uint8_t* pso, const uint8_t* desired, uint8_t* applied, vr::Report* out) {
    __try {
        *out = vr::resync(pso, desired, applied);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The failing sequence up to the flush that matters, with the repair (`hook`) as asked. Returns slot 0's final bind.
struct Replay {
    Bound bound;
    vr::Report report;
    bool faulted = false;
};
Replay failingSequence(FakeList& f, bool hook, uint32_t queueOffset = 0) {
    Replay r;
    f.setLayout(1, 20);                          // the first eye's lighting resolve: one slot, stride 20
    f.setVertexBuffer(0, f.wrapperPtr(0), queueOffset);   // Q
    f.flush();                                   // binds Q
    f.setLayout(0, 0);                           // two zero-slot full-screen draws
    f.dirty(0x1000);
    f.flush();
    f.dirty(0x1000);
    f.flush();                                   // applied[0] zeroed, (NULL, 0, 0) bound; desired[0] is still Q
    f.setLayout(1, 20);                          // the second eye's resolve
    f.setVertexBuffer(0, f.wrapperPtr(0), queueOffset);   // equals desired: SKIPPED
    f.dirty(0x1000);                             // the layout changed: the flush proceeds
    if (hook) r.faulted = !tryResync(f.pso, f.desired(), f.applied(), &r.report);
    f.flush();
    r.bound = f.lastBind.empty() ? Bound{} : f.lastBind[0];
    return r;
}

// ---- V1 ----------------------------------------------------------------------------------------------------------------------------------------------------
void caseReplay() {
    {
        FakeList f;
        const Replay r = failingSequence(f, false);
        check(r.bound.buffer == 0 && r.bound.stride == 20 && r.bound.offset == 0,
              "V1.a the model reproduces the field failure: stock, the second eye's resolve is bound (NULL, stride 20, offset 0)");
    }
    {
        FakeList f;
        const Replay r = failingSequence(f, true, 24);
        check(!r.faulted && r.bound.buffer == kNativeQ && r.bound.stride == 20 && r.bound.offset == 24,
              "V1.b the repair on the same sequence binds Q's native buffer, stride 20, and the desired offset (24): applied buffer and offset were rewritten");
        check(r.report.layoutCount == 1 && r.report.checked == 1 && r.report.repaired == 1 && r.report.hasFirst && r.report.first.slot == 0 &&
                  r.report.first.native == kNativeQ && r.report.first.applied == 0 && r.report.first.wrapper == reinterpret_cast<uintptr_t>(f.wrapperPtr(0)),
              "V1.c ...and the report names it: one slot checked, one repaired, slot 0, the wrapper, Q's native buffer, the applied value it found (NULL)");
        const Replay again = failingSequence(f, true, 24);
        check(again.bound.buffer == kNativeQ && again.report.repaired == 1, "V1.d ...and the whole sequence again (the next frame) is repaired again");
    }
    {
        FakeList f;   // a healthy list: the buffer is bound and stays bound
        f.setLayout(1, 20);
        f.setVertexBuffer(0, f.wrapperPtr(0), 0);
        f.flush();
        vr::Report r;
        check(tryResync(f.pso, f.desired(), f.applied(), &r) && r.layoutCount == 1 && r.checked == 1 && r.repaired == 0 && !r.hasFirst,
              "V1.e a healthy list (applied = the wrapper's native buffer): one slot checked, nothing repaired, nothing written");
    }
    {
        FakeList f;   // slots at or above the layout's count are left alone
        f.setLayout(1, 20);
        f.setVertexBuffer(0, f.wrapperPtr(0), 0);
        f.setVertexBuffer(1, f.wrapperPtr(1), 8);
        f.flush();
        vr::store64(f.applied() + vr::kAppliedBuffers + 8, 0);   // slot 1's applied buffer is stale, but the layout declares one slot
        vr::store32(f.applied() + vr::kAppliedOffsets + 4, 77);
        vr::Report r;
        check(tryResync(f.pso, f.desired(), f.applied(), &r) && r.checked == 1 && r.repaired == 0 && f.appliedBuffer(1) == 0 && f.appliedOffset(1) == 77,
              "V1.f a slot at or above the layout's count (slot 1 of a one-slot layout) is not looked at and not written");
    }
    {
        FakeList f;   // a null desired slot is left alone, whatever the applied slot holds
        f.setLayout(2, 20);
        f.setVertexBuffer(1, f.wrapperPtr(1), 0);
        f.flush();
        vr::store64(f.applied() + vr::kAppliedBuffers, kNativeS);   // slot 0 has no desired wrapper but an applied buffer
        vr::store32(f.applied() + vr::kAppliedOffsets, 5);
        vr::Report r;
        check(tryResync(f.pso, f.desired(), f.applied(), &r) && r.checked == 1 && r.repaired == 0 && f.appliedBuffer(0) == kNativeS && f.appliedOffset(0) == 5,
              "V1.g a null desired wrapper is not dereferenced, not counted and not written");
    }
    {
        FakeList f;   // several slots: two stale, one healthy
        f.setLayout(3, 20);
        f.setVertexBuffer(0, f.wrapperPtr(0), 4);
        f.setVertexBuffer(1, f.wrapperPtr(1), 8);
        f.setVertexBuffer(2, f.wrapperPtr(2), 12);
        f.flush();
        vr::store64(f.applied() + vr::kAppliedBuffers + 16, 0);
        vr::store64(f.applied() + vr::kAppliedBuffers + 0, kNativeS);   // slot 0 holds a DIFFERENT buffer, not NULL
        vr::Report r;
        check(tryResync(f.pso, f.desired(), f.applied(), &r) && r.checked == 3 && r.repaired == 2 && r.first.slot == 0 && r.first.applied == kNativeS &&
                  f.appliedBuffer(0) == kNativeQ && f.appliedBuffer(1) == kNativeR && f.appliedBuffer(2) == kNativeS && f.appliedOffset(0) == 4 && f.appliedOffset(2) == 12,
              "V1.h three slots, two stale (one NULL, one a different buffer): both repaired to their wrappers' native buffers and desired offsets, the healthy one untouched, "
              "and the report names the first");
    }
    {
        FakeList f;   // a layout of 40 slots: the loop stops at 16, the arrays' length
        f.setLayout(40, 20);
        for (int i = 0; i < 16; ++i) f.setVertexBuffer(static_cast<uint32_t>(i), f.wrapperPtr(0), 0);
        f.flush();
        // slot 16 would be the desired offsets' first word and the applied strides' first word: make both look like a stale wrapper-and-buffer pair
        vr::store64(f.desired() + vr::kDesiredWrappers + 8 * 16, reinterpret_cast<uintptr_t>(f.wrapperPtr(1)));
        vr::store64(f.applied() + vr::kAppliedBuffers + 8 * 16, 0);
        uint8_t beyondBefore[8];
        std::memcpy(beyondBefore, f.applied() + vr::kAppliedBuffers + 8 * 16, 8);
        vr::Report r;
        check(tryResync(f.pso, f.desired(), f.applied(), &r) && r.layoutCount == 40 && r.checked == 16 && r.repaired == 0 &&
                  std::memcmp(beyondBefore, f.applied() + vr::kAppliedBuffers + 8 * 16, 8) == 0,
              "V1.i a layout that declares more than 16 slots is clamped to 16: nothing past the arrays is read as a wrapper or written");
    }
    {
        vr::Report r;
        FakeList f;
        f.setLayout(1, 20);
        f.setVertexBuffer(0, f.wrapperPtr(0), 0);
        bool ok = tryResync(nullptr, f.desired(), f.applied(), &r) && r.checked == 0;
        ok = ok && tryResync(f.pso, nullptr, f.applied(), &r) && r.checked == 0;
        ok = ok && tryResync(f.pso, f.desired(), nullptr, &r) && r.checked == 0;
        check(ok, "V1.j a null pso, a null desired state and a null applied cache read nothing and write nothing");
        vr::store64(f.pso + vr::kPsoLayout, 0);
        check(tryResync(f.pso, f.desired(), f.applied(), &r) && r.layoutCount == 0 && r.checked == 0, "V1.k a pso with no layout object reads nothing further");
        FakeList g;
        g.setLayout(0, 0);
        check(tryResync(g.pso, g.desired(), g.applied(), &r) && r.layoutCount == 0 && r.checked == 0 && !r.hasFirst, "V1.l a layout with no slots (the zero-slot draws) checks nothing");
    }
}

// ---- V2 ----------------------------------------------------------------------------------------------------------------------------------------------------
void caseGate() {
    // The bytes spelled out again here, not copied from the header: build 332841's FlushIA at EliteDangerous64.exe+0x522A50, read from the exe.
    const uint8_t exe[16] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x49, 0x8B, 0xF8};
    check(vr::prologueMatches(exe, 16), "V2.a the 16 bytes read from the exe are the prologue the hook checks");
    bool everyByteMatters = true;
    for (size_t i = 0; i < 16; ++i) {
        uint8_t wrong[16];
        std::memcpy(wrong, exe, 16);
        wrong[i] ^= 0x01;
        everyByteMatters = everyByteMatters && !vr::prologueMatches(wrong, 16);
    }
    check(everyByteMatters, "V2.b a prologue that differs in any one of the 16 bytes is refused");
    check(!vr::prologueMatches(exe, 15) && !vr::prologueMatches(nullptr, 16), "V2.c a short or missing buffer is refused");
    check(vr::kFlushIaRva == 0x522A50u && vr::kPrologueBytes == 16, "V2.d the RVA is 0x522A50 (0x140522A50) and the check covers 16 bytes");
    check(vr::identityMatches(1788384820u, 104894464u), "V2.e the PE pair of build 332841 passes");
    check(!vr::identityMatches(1788384821u, 104894464u) && !vr::identityMatches(1788384820u, 104894465u) && !vr::identityMatches(0, 0),
          "V2.f a different timestamp, a different image size or neither is refused");
}

// ---- V3 ----------------------------------------------------------------------------------------------------------------------------------------------------
void caseInstruments() {
    char line[600];
    vr::formatResyncLine(line, sizeof(line), 7);
    check(std::strcmp(line, "vertex resync: 7 stale vertex-buffer bindings in the last 60 s (repaired)") == 0, "V3.a the 60 s line is exactly the one asked for");
    vr::formatHeartbeatLine(line, sizeof(line), 0, 0);
    check(std::strcmp(line, "vertex resync: armed; 0 stale vertex-buffer bindings repaired in the last 10 min (0 flushes seen)") == 0,
          "V3.b the heartbeat with zero counts is exactly the line asked for");
    vr::formatHeartbeatLine(line, sizeof(line), 12, 5000000000ull);
    check(std::strcmp(line, "vertex resync: armed; 12 stale vertex-buffer bindings repaired in the last 10 min (5000000000 flushes seen)") == 0,
          "V3.c ...and with counts, the flush count in full (past 32 bits)");
    vr::formatSessionLine(line, sizeof(line), 12, 5000000000ull);
    check(std::strcmp(line, "vertex resync: armed; 12 stale vertex-buffer bindings repaired this session (5000000000 flushes seen)") == 0, "V3.d the session line is exactly the one asked for");
    vr::Sighting s;
    s.slot = 2; s.wrapper = 0xABC0; s.native = 0xDEF0; s.applied = 0x0;
    vr::formatSightingLine(line, sizeof(line), 3, s, 0x7F0000, 1, 4242, 2, "0x522A50/0x510404");
    check(has(line, "vertex resync: stale binding 3 of 8: slot 2, list 0x7F0000, desired wrapper 0xABC0, native buffer 0xDEF0, applied buffer 0x0, layout slots 1, thread 4242, repaired; "
                    "game stack (2 frames) 0x522A50/0x510404"),
          "V3.e a first-sighting line carries the slot, the list, the desired wrapper, the native buffer, the applied value, the layout count, the thread and the game stack");
    vr::formatSightingLine(line, sizeof(line), 1, s, 1, 1, 1, 0, "");
    check(has(line, "(none in the game's image)"), "V3.f ...and the empty stack is said so");

    vr::WindowCount w;
    w.add(5);
    check(w.take(1000) == 0, "V3.g the first tick starts the window and says nothing");
    check(w.take(60999) == 0, "V3.h a tick inside the 60 s says nothing");
    check(w.take(61000) == 5 && w.pending() == 0, "V3.i the tick 60 s after the first says the count and empties it");
    w.add(2);
    check(w.take(61001) == 0 && w.take(120999) == 0 && w.take(121000) == 2, "V3.j the window restarts when it ends: the next count is said 60 s after the last, not before");
    check(w.take(181000) == 0, "V3.k a window with nothing in it says nothing (the 60 s line is only for a non-zero count)");
    check(vr::kWindowMs == 60000 && vr::kHeartbeatMs == 600000, "V3.l the windows are 60 s and 10 min");

    vr::WindowCount beat(vr::kHeartbeatMs);
    uint32_t n = 99;
    check(!beat.tick(5000, &n), "V3.m the first tick of the ten-minute window starts it and ends nothing");
    check(!beat.tick(604999, &n), "V3.n a tick one millisecond short of ten minutes ends nothing");
    n = 99;
    check(beat.tick(605000, &n) && n == 0, "V3.o ten minutes with nothing in them end the window and report 0: zero counts are said");
    beat.add(4);
    n = 99;
    check(!beat.tick(1204999, &n) && beat.tick(1205000, &n) && n == 4, "V3.p the next window reports its own 4, ten minutes after the last");
    n = 99;
    check(beat.tick(1805000, &n) && n == 0, "V3.q ...and is emptied by it: the one after reports 0, not 4 again");

    vr::SightingGate gate;
    unsigned taken = 0;
    for (int i = 0; i < 20; ++i) taken += gate.take() ? 1u : 0u;
    check(taken == 8 && gate.taken() == 8 && vr::kMaxSightings == 8, "V3.r twenty first sightings are eight lines");

    vr::FlushCount flushes;
    check(flushes.read() == 0, "V3.s a new flush counter reads 0");
    for (int i = 0; i < 5; ++i) flushes.bump();
    check(flushes.read() == 5, "V3.t five flushes count 5: one each");
    flushes.reset();
    check(flushes.read() == 0, "V3.u ...and reset starts over");
    check(sizeof(vr::FlushCount) == 64 && alignof(vr::FlushCount) == 64, "V3.v the flush counter owns a 64-byte cache line (no other global shares the line the render thread writes)");
}

// ---- V4: the production hook on a synthetic FlushIA --------------------------------------------------------------------------------------------------------
// The real 16-byte prologue, then "mov rax,[r9+0x168]" (applied[0] AS THE ORIGINAL SEES IT) and the real epilogue: mov rbx,[rsp+0x60]; add rsp,0x30; pop r14; pop rdi; pop rsi; ret.
uint8_t* makeSyntheticFlush(const uint8_t* prologue) {
    auto* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!p) return nullptr;
    const uint8_t tail[] = {0x49, 0x8B, 0x81, 0x68, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x5C, 0x24, 0x60, 0x48, 0x83, 0xC4, 0x30, 0x41, 0x5E, 0x5F, 0x5E, 0xC3};
    std::memcpy(p, prologue, 16);
    std::memcpy(p + 16, tail, sizeof(tail));
    return p;
}
using FlushFn = uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

uint8_t* g_fn = nullptr;   // the synthetic FlushIA the hook is installed on

size_t countLinesWith(const char* needle) {
    size_t n = 0;
    for (size_t i = 0; i < edvr::vertexResyncTestLineCount(); ++i) n += std::strstr(edvr::vertexResyncTestLine(i), needle) ? 1u : 0u;
    return n;
}
// The lines said, for a failure's message.
std::string lastLineWith(const char* needle) {
    std::string found;
    for (size_t i = 0; i < edvr::vertexResyncTestLineCount(); ++i)
        if (std::strstr(edvr::vertexResyncTestLine(i), needle)) found = edvr::vertexResyncTestLine(i);
    return found;
}
bool lineIs(const char* needle, const char* exact) { return countLinesWith(exact) == 1 && lastLineWith(needle) == exact; }

uintptr_t callFlush(FlushFn fn, FakeList& f) { return fn(reinterpret_cast<uintptr_t>(f.pso), 0, reinterpret_cast<uintptr_t>(f.desired()), reinterpret_cast<uintptr_t>(f.applied())); }
// A stale-slot-0 list to call the synthetic function with.
void stale(FakeList& f, uint32_t offset = 0) {
    f.setLayout(1, 20);
    f.setVertexBuffer(0, f.wrapperPtr(0), offset);
    vr::store64(f.applied() + vr::kAppliedBuffers, 0);   // zeroed by a zero-slot flush
    vr::store32(f.applied() + vr::kAppliedOffsets, 0);
}
void staleCalls(FlushFn call, int times) {
    for (int i = 0; i < times; ++i) {
        FakeList f;
        stale(f);
        callFlush(call, f);
    }
}
void healthyCalls(FlushFn call, int times) {
    for (int i = 0; i < times; ++i) {
        FakeList g;
        g.setLayout(1, 20);
        g.setVertexBuffer(0, g.wrapperPtr(0), 0);
        callFlush(call, g);
    }
}

void caseHook() {
    using namespace edvr;
    const uint8_t exe[16] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x49, 0x8B, 0xF8};
    {
        // V4.a nothing exists before the hook: the synthetic function returns the stale applied[0]
        uint8_t* p = makeSyntheticFlush(exe);
        FakeList f;
        stale(f);
        check(p && callFlush(reinterpret_cast<FlushFn>(p), f) == 0, "V4.a before the hook the synthetic flush sees the stale cache (applied[0] = NULL): the control");
    }
    // V4.b a function whose prologue is not build 332841's is refused: one line, nothing patched, and it is not retried
    uint8_t wrongBytes[16];
    std::memcpy(wrongBytes, exe, 16);
    wrongBytes[6] ^= 0x01;
    uint8_t* wrongFn = makeSyntheticFlush(wrongBytes);
    vertexResyncTestReset();
    vertexResyncTestSetTarget(reinterpret_cast<uintptr_t>(wrongFn));
    uint8_t before[64];
    std::memcpy(before, wrongFn, 64);
    vertexResyncInstall();
    check(wrongFn && std::memcmp(before, wrongFn, 64) == 0 && !vertexResyncTestState().installed && countLinesWith("NOT installed -- the 16 bytes") == 1,
          "V4.b a function whose prologue is not build 332841's is refused: one line, nothing patched, not installed");
    const size_t linesAfterRefusal = vertexResyncTestLineCount();
    vertexResyncInstall();
    check(vertexResyncTestLineCount() == linesAfterRefusal && vertexResyncTestState().attempted, "V4.c a refusal is final: asking again says nothing and tries nothing");
    {
        // Nothing armed says nothing: no heartbeat through a ten-minute window, no session line, no process-exit line.
        char out[300] = "unchanged";
        vertexResyncPoll(1000);
        vertexResyncPoll(601000);
        vertexResyncShutdown();
        check(countLinesWith("vertex resync: armed;") == 0 && countLinesWith("stale vertex-buffer bindings") == 0 && vertexResyncTestLineCount() == linesAfterRefusal &&
                  vertexResyncTestExitLine(out, sizeof(out)) == 0,
              "V4.d a hook that never armed says no heartbeat, no session line and no exit line: the heartbeat is for an armed hook only");
    }

    // V4.e the right function arms (stolen = the first instruction, 5 bytes)
    uint8_t* fn = makeSyntheticFlush(exe);
    g_fn = fn;
    vertexResyncTestReset();
    vertexResyncTestSetTarget(reinterpret_cast<uintptr_t>(fn));
    vertexResyncInstall();
    check(vertexResyncTestState().installed && countLinesWith("vertex resync: hook armed") == 1 && countLinesWith("stolen=5 bytes") == 1 && countLinesWith("prologue 16/16 bytes verified") == 1 &&
              countLinesWith("a heartbeat every 10 min (zero counts included) and a line when the session ends") == 1 && fn[0] == 0xE9,
          "V4.e the right prologue arms the hook: the line names it (stolen = 5 bytes, 16/16 verified, the heartbeat and the session line promised) and the entry is a jump");
    const size_t armedLines = vertexResyncTestLineCount();
    vertexResyncInstall();
    check(vertexResyncTestLineCount() == armedLines, "V4.f installing twice changes nothing");

    const FlushFn call = reinterpret_cast<FlushFn>(fn);
    {
        FakeList f;
        stale(f, 24);
        const uintptr_t seen = callFlush(call, f);
        const VertexResyncTestState s = vertexResyncTestState();
        check(seen == kNativeQ && f.appliedBuffer(0) == kNativeQ && f.appliedOffset(0) == 24 && s.flushes == 1 && s.repaired == 1,
              "V4.g the original runs AFTER the repair: with the cache stale, the synthetic flush reads Q's native buffer from applied[0]; the offset was copied; 1 flush, 1 repaired");
        check(countLinesWith("vertex resync: stale binding 1 of 8: slot 0, list 0x") == 1 && countLinesWith("native buffer 0x1000000010, applied buffer 0x0, layout slots 1") == 1 &&
                  countLinesWith("repaired; game stack") == 1,
              "V4.h the first sighting is logged: slot, list, wrapper, native buffer, applied value, layout count, repaired, and the game stack");
        FakeList g;   // a healthy call changes nothing
        g.setLayout(1, 20);
        g.setVertexBuffer(0, g.wrapperPtr(0), 0);
        const size_t lines = vertexResyncTestLineCount();
        check(callFlush(call, g) == kNativeQ && vertexResyncTestState().repaired == 1 && vertexResyncTestState().flushes == 2 && vertexResyncTestLineCount() == lines,
              "V4.i a healthy call is forwarded untouched and counted as a flush: nothing repaired, no line");
    }
    {
        // twelve more stale calls: eight sightings in all, however many repairs
        staleCalls(call, 12);
        check(vertexResyncTestState().sightings == 8 && countLinesWith("vertex resync: stale binding ") == 8 && vertexResyncTestState().repaired == 13 &&
                  vertexResyncTestState().flushes == 14,
              "V4.j the first-sighting lines stop at eight, while the repairs (13) and the flushes (14) keep counting");
    }
}

// ---- V5: what the log says while armed ---------------------------------------------------------------------------------------------------------------------
std::wstring tempDir() {
    wchar_t tmp[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH) return L"";
    return std::wstring(tmp) + L"edvr_vertex_resync_test_" + std::to_wstring(GetCurrentProcessId());
}
bool readBytes(const std::wstring& path, std::string* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[4096];
    DWORD got = 0;
    out->clear();
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) out->append(buf, got);
    CloseHandle(h);
    return true;
}

// The process-exit path with the real Log: the game never unloads this DLL, so its session line is written by Log::detachDuringProcessExit through the callback the hook
// registered when it armed. Writes one file in the temp folder and removes it.
void checkRealLogExit(const char* sessionLine) {
    using namespace edvr;
    const std::wstring dir = tempDir();
    Config::get().set("log.enabled", "1");
    const bool opened = !dir.empty() && Log::get().open(dir, L"vresync");
    check(opened, "V5.m a real log opens in the temp folder");
    if (!opened) return;
    Log::get().note("%s", "V5 marker: the last line written before the process ends");
    Log::get().detachDuringProcessExit();
    std::string text;
    bool read = false;
    WIN32_FIND_DATAW found;
    const std::wstring pattern = dir + L"\\edvr_vresync_*.log";
    HANDLE find = FindFirstFileW(pattern.c_str(), &found);
    std::wstring path;
    if (find != INVALID_HANDLE_VALUE) {
        path = dir + L"\\" + found.cFileName;
        read = readBytes(path, &text);
        FindClose(find);
    }
    check(read, "V5.n the process-exit path left a log file to read");
    const std::string want = std::string("] ") + sessionLine + "\r\n";
    const size_t marker = text.find("V5 marker"), exitAt = text.find(want);
    check(marker != std::string::npos && exitAt != std::string::npos && marker < exitAt,
          "V5.o the session line is in the log, after everything written before the process ended");
    check(exitAt != std::string::npos && text.find(want, exitAt + 1) == std::string::npos && exitAt >= 13 && text[exitAt - 13] == '[' && text[exitAt - 10] == ':' && text[exitAt - 4] == '.',
          "V5.p ...once, stamped like every other line ([hh:mm:ss.mmm]), and ended with a line break");
    if (read) DeleteFileW(path.c_str());
    RemoveDirectoryW(dir.c_str());
}

void caseReport() {
    using namespace edvr;
    // The hook stays in the synthetic function from caseHook; the counts and the windows start over.
    vertexResyncTestReset();
    const FlushFn call = reinterpret_cast<FlushFn>(g_fn);
    vertexResyncPoll(1000);   // starts the 60 s and the ten-minute windows
    check(vertexResyncTestLineCount() == 0, "V5.a the poll that starts the windows says nothing");
    staleCalls(call, 7);
    healthyCalls(call, 3);   // ten flushes, seven of them stale
    vertexResyncPoll(60999);
    check(countLinesWith("stale vertex-buffer bindings in the last 60 s") == 0, "V5.b a poll inside the 60 s says nothing about the count");
    vertexResyncPoll(61000);
    check(lineIs("in the last 60 s", "vertex resync: 7 stale vertex-buffer bindings in the last 60 s (repaired)"), "V5.c the poll 60 s after the first says the count: seven, repaired");
    vertexResyncPoll(121000);
    check(countLinesWith("stale vertex-buffer bindings in the last 60 s") == 1, "V5.d a window with nothing in it says nothing: the 60 s line is for a non-zero count only");
    vertexResyncPoll(600999);
    check(countLinesWith("repaired in the last 10 min") == 0, "V5.e no heartbeat a millisecond short of ten minutes");
    vertexResyncPoll(601000);
    check(lineIs("repaired in the last 10 min", "vertex resync: armed; 7 stale vertex-buffer bindings repaired in the last 10 min (10 flushes seen)"),
          "V5.f ten minutes after the first poll the heartbeat says 7 repaired and 10 flushes seen");
    check(countLinesWith("stale vertex-buffer bindings in the last 60 s") == 1, "V5.g ...and the 60 s line did not repeat it");
    vertexResyncPoll(1201000);
    check(countLinesWith("repaired in the last 10 min") == 2 && lastLineWith("repaired in the last 10 min") == "vertex resync: armed; 0 stale vertex-buffer bindings repaired in the last 10 min (0 flushes seen)",
          "V5.h a quiet ten minutes still say it, with zero counts: armed, 0 repaired, 0 flushes seen");
    healthyCalls(call, 5);
    vertexResyncPoll(1801000);
    check(countLinesWith("repaired in the last 10 min") == 3 && lastLineWith("repaired in the last 10 min") == "vertex resync: armed; 0 stale vertex-buffer bindings repaired in the last 10 min (5 flushes seen)",
          "V5.i five healthy flushes make 0 repaired, 5 flushes seen: the heartbeat counts its own window, not the session");
    staleCalls(call, 3);
    vertexResyncPoll(2401000);
    check(countLinesWith("repaired in the last 10 min") == 4 && lastLineWith("repaired in the last 10 min") == "vertex resync: armed; 3 stale vertex-buffer bindings repaired in the last 10 min (3 flushes seen)",
          "V5.j three stale flushes make 3 repaired, 3 flushes seen: not the 10 repaired and 18 flushes since the start");
    check(countLinesWith("vertex resync: 3 stale vertex-buffer bindings in the last 60 s (repaired)") == 1,
          "V5.k the 60 s count keeps running beside the heartbeat");

    const char* const sessionLine = "vertex resync: armed; 10 stale vertex-buffer bindings repaired this session (18 flushes seen)";
    const size_t before = vertexResyncTestLineCount();
    vertexResyncShutdown();
    check(vertexResyncTestLineCount() == before + 1 && lineIs("this session", sessionLine),
          "V5.l shutdown says the session totals once: 10 repaired and 18 flushes seen since the hook armed");
    char out[300];
    const int n = vertexResyncTestExitLine(out, sizeof(out));
    check(n == static_cast<int>(std::strlen(sessionLine)) && std::strcmp(out, sessionLine) == 0, "V5.q the process-exit callback returns the same line, and its length");
    if (g_writesFiles) checkRealLogExit(sessionLine);
}

// ---- V6: faults and the stand-down -------------------------------------------------------------------------------------------------------------------------
void caseFaults() {
    using namespace edvr;
    const FlushFn call = reinterpret_cast<FlushFn>(g_fn);
    {
        // a wild layout pointer: the repair faults inside the hook, SEH absorbs it, the original still runs
        FakeList f;
        stale(f);
        vr::store64(f.pso + vr::kPsoLayout, 0x10);
        const uint64_t faults = vertexResyncTestState().faults;
        const uintptr_t seen = callFlush(call, f);
        check(seen == 0 && vertexResyncTestState().faults == faults + 1 && countLinesWith("faulted and was absorbed") == 1,
              "V6.a a read that faults inside the hook is absorbed and counted, one line, and the game's flush runs as it would have");
        for (int i = 0; i < 10; ++i) {
            FakeList g;
            stale(g);
            vr::store64(g.pso + vr::kPsoLayout, 0x10);
            callFlush(call, g);
        }
        check(vertexResyncTestState().faults == 8 && countLinesWith("stood down after eight faults") == 1 && vertexResyncTestState().flushes == 18 + 8,
              "V6.b eight faults stand the hook down (one line) and the relay then forwards straight to the original: the later calls are not even counted");
        FakeList h;
        stale(h);
        check(callFlush(call, h) == 0, "V6.c ...a stale call after the stand-down is not repaired");
    }
    {
        // A stood-down hook is not an armed one: no heartbeat, no session line, no exit line.
        const size_t heartbeats = countLinesWith("repaired in the last 10 min");
        const size_t sessions = countLinesWith("repaired this session");
        vertexResyncPoll(3001000);
        vertexResyncPoll(3601000);
        vertexResyncShutdown();
        char out[300] = "unchanged";
        check(countLinesWith("repaired in the last 10 min") == heartbeats && countLinesWith("repaired this session") == sessions && vertexResyncTestExitLine(out, sizeof(out)) == 0,
              "V6.d after the stand-down the heartbeat, the session line and the process-exit line all say nothing");
    }
}

// ---- V7: the glue, read as text ----------------------------------------------------------------------------------------------------------------------------
void caseGlue(const std::string& root) {
    if (root.empty()) return;
    std::string device, hook, core;
    const bool readAll = readText(root + "/src/d3d11/device_hook.cpp", &device) && readText(root + "/src/d3d11/vertex_resync_hook.cpp", &hook) &&
                         readText(root + "/src/d3d11/vertex_resync_core.h", &core);
    check(readAll, "V7.p0 device_hook.cpp, vertex_resync_hook.cpp and vertex_resync_core.h can be read under the repository root");
    if (!readAll) return;
    check(occurrences(device, "vertexResyncInstall();") == 1 && precedes(device, "flatTemporalStart(device);", "vertexResyncInstall();") &&
              precedes(device, "vertexResyncInstall();", "armFlipTimeline(sentinelCfg, table, span, \"context\");"),
          "V7.a the hook is installed once, at startup, after the other installers and in either profile (no profile test around it)");
    check(occurrences(device, "vertexResyncPoll(stampMs());") == 1 && precedes(device, "vScreenRefreshConfig();", "vertexResyncPoll(stampMs());"),
          "V7.b the 60 s count and the heartbeat are asked once a second, in the config tick, after the reload");
    check(occurrences(device, "vertexResyncShutdown();") == 1 && precedes(device, "uiPanelScaleShutdown();", "vertexResyncShutdown();"),
          "V7.c a FreeLibrary teardown says the session line, once, in the shutdown sequence");
    check(occurrences(hook, "Log::get().setExitLine(&exitLine);") == 1 && precedes(hook, "g_installed.store(true, std::memory_order_release);", "Log::get().setExitLine(&exitLine);"),
          "V7.d the hook registers its process-exit line with the log, once, when it arms");
    check(occurrences(hook, "g_flushes.bump();") == 1 && precedes(hook, "g_flushes.bump();", "if (!guardedResync(pso, desired, applied, &r))") &&
              occurrences(hook, "return forward(pso, context, desired, applied);") == 1,
          "V7.e the callback counts the flush first, repairs under SEH, and forwards to the original");
    check(!has(hook, "Config::get") && !has(hook, "getString(") && !has(hook, "getBool("), "V7.f the hook reads no key: the repair is always on");
    const size_t start = core.find("struct alignas(64) FlushCount {");
    const size_t end = start == std::string::npos ? start : core.find("\n};", start);   // the struct's own closing brace, not the "{0};" of its member
    const std::string counter = start == std::string::npos || end == std::string::npos ? std::string() : core.substr(start, end - start);
    check(!counter.empty() && occurrences(counter, "n.store(n.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);") == 1 && !has(counter, "fetch_add") &&
              !has(counter, "exchange") && !has(counter, "++"),
          "V7.g the flush counter is a relaxed load and a relaxed store: no lock prefix, no read-modify-write on the render thread's path");
}
}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false;
    std::string root;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--self-test") selfTest = true;
        else if (a == "--dry-run") { selfTest = true; g_writesFiles = false; }
        else if (i == 2 && argv[1] == std::string("--self-test")) root = a;
        else {
            std::fprintf(stderr, "usage: vertex_resync_test --self-test [repository root] | --dry-run\n");
            return 2;
        }
    }
    if (!selfTest) {
        std::fprintf(stderr, "usage: vertex_resync_test --self-test [repository root] | --dry-run\n");
        return 2;
    }
    caseReplay();
    caseGate();
    caseInstruments();
    caseHook();
    caseReport();
    caseFaults();
    caseGlue(root);
    if (g_failures) {
        std::printf("FAIL: vertex resync: %u of %u checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: %u vertex resync checks (%zu cases:", g_checks, sizeof(kCases) / sizeof(kCases[0]));
    for (const char* c : kCases) std::printf(" %s", c);
    std::printf(")\n");
    return 0;
}
