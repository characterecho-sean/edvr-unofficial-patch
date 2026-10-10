// vertex_resync_test: the game's stale vertex-buffer binding and the repair at the entry of Frontier's input-assembler flush (src/d3d11/vertex_resync_core.h,
// vertex_resync_hook.cpp; docs/scanner-body.md, "The root cause: Frontier's f3d state cache").
//
//   --dry-run    the same run (the rig never writes a file), for the gate's --dry-run convention
//   --self-test  every check; the optional argument is the repository root, where the glue is read as text (the mutation tool hands it a temp root holding an edited copy)
//
// The pure half runs on a fake command list laid out at the real offsets (a model of SetVertexBuffer 0x1404E9D70 and of FlushIA's slot loop 0x140522A50, written from the
// disassembly in analysis\decomp\scanner_vb_dump_*). The hook half is the production hook.cpp compiled with EDVR_VERTEX_RESYNC_TEST: the real CodeHook patches a synthetic
// function that begins with the real 16-byte prologue of FlushIA, the real relay runs, the real SEH-guarded repair runs before the original. Cases ("V<case>.<what>";
// mutants.py names the case that must catch each mutation):
//   V1  the failing sequence replayed (resolve binds Q; two zero-slot draws zero the applied slot; the second eye's SetVertexBuffer(Q) is skipped): stock binds (NULL, 20, 0);
//       with the repair on it binds Q with the desired offset; with it off the bytes are not written but the desync is counted; slots at or above the layout's count, a
//       null desired slot, a count above 16, a null pointer anywhere, and a healthy list are left alone
//   V2  the gate and the prologue: the PE pair, the 16 bytes (every position), the length
//   V3  the key: the spellings of off, the default, and the flat profile's allow-list
//   V4  the instruments: the exact lines, the 60 s window (non-zero only), the eight first sightings
//   V5  the hook end to end on a synthetic FlushIA: a wrong prologue is refused with a line and nothing patched; the right one arms (stolen = 5); the original sees the repaired
//       cache; off counts only; the sightings are capped; a fault is absorbed and eight stand it down; nothing is retried; the live key and the 60 s line through the poll
//   V6  the glue, read as text: device_hook installs once after the other startup installers and polls once a second; resolve_bind_fix counts its lends; the hook reads the key
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "config.h"
#include "runtime_profile.h"
#include "vertex_resync_core.h"
#include "vertex_resync_hook.h"

namespace edvr {
void breadcrumb(const char*) {}   // production guard.cpp's crash-channel dependency (proxy.cpp), as the other rigs stub it
}  // namespace edvr

namespace vr = edvr::vresync;

// The case ids the checks label themselves with (tools\vertex_resync_test\mutants.py reads the labels).
static const char* const kCases[] = {"V1.replay", "V2.gate", "V3.key", "V4.instruments", "V5.hook", "V6.glue"};

namespace {
unsigned g_checks = 0, g_failures = 0;
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
size_t at(const std::string& text, const char* needle) { return text.find(needle); }
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

__declspec(noinline) bool tryResync(const uint8_t* pso, const uint8_t* desired, uint8_t* applied, bool repair, vr::Report* out) {
    __try {
        *out = vr::resync(pso, desired, applied, repair);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The failing sequence up to the flush that matters, with the hook (`hook`) and the repair (`repair`) as asked. Returns slot 0's final bind.
struct Replay {
    Bound bound;
    vr::Report report;
    bool faulted = false;
};
Replay failingSequence(FakeList& f, bool hook, bool repair, uint32_t queueOffset = 0) {
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
    if (hook) r.faulted = !tryResync(f.pso, f.desired(), f.applied(), repair, &r.report);
    f.flush();
    r.bound = f.lastBind.empty() ? Bound{} : f.lastBind[0];
    return r;
}

// ---- V1 ----------------------------------------------------------------------------------------------------------------------------------------------------
void caseReplay() {
    {
        FakeList f;
        const Replay r = failingSequence(f, false, true);
        check(r.bound.buffer == 0 && r.bound.stride == 20 && r.bound.offset == 0,
              "V1.a the model reproduces the field failure: stock, the second eye's resolve is bound (NULL, stride 20, offset 0)");
    }
    {
        FakeList f;
        const Replay r = failingSequence(f, true, true, 24);
        check(!r.faulted && r.bound.buffer == kNativeQ && r.bound.stride == 20 && r.bound.offset == 24,
              "V1.b with the repair on the same sequence binds Q's native buffer, stride 20, and the desired offset (24): applied buffer and offset were rewritten");
        check(r.report.layoutCount == 1 && r.report.checked == 1 && r.report.desynced == 1 && r.report.repaired == 1 && r.report.hasFirst && r.report.first.slot == 0 &&
                  r.report.first.native == kNativeQ && r.report.first.applied == 0 && r.report.first.wrapper == reinterpret_cast<uintptr_t>(f.wrapperPtr(0)),
              "V1.c ...and the report names it: one slot checked, one desynced, one repaired, slot 0, the wrapper, Q's native buffer, the applied value it found (NULL)");
        const Replay again = failingSequence(f, true, true, 24);
        check(again.bound.buffer == kNativeQ && again.report.desynced == 1, "V1.d ...and the whole sequence again (the next frame) is repaired again");
    }
    {
        FakeList f;
        const Replay r = failingSequence(f, true, false, 24);
        check(!r.faulted && r.report.desynced == 1 && r.report.repaired == 0 && r.bound.buffer == 0 && r.bound.stride == 20 && r.bound.offset == 0,
              "V1.e with the repair off the desync is counted (1), nothing is repaired (0), and the draw is bound as the game had it: (NULL, 20, 0)");
        FakeList g;
        const Replay before = failingSequence(g, false, true, 24);
        FakeList h;
        failingSequence(h, true, false, 24);
        check(std::memcmp(g.applied(), h.applied(), sizeof(g.list) - vr::kListApplied) == 0 && before.bound.buffer == 0,
              "V1.f ...and not one byte of the applied cache differs from the stock run's: off writes nothing");
    }
    {
        FakeList f;   // a healthy list: the buffer is bound and stays bound
        f.setLayout(1, 20);
        f.setVertexBuffer(0, f.wrapperPtr(0), 0);
        f.flush();
        vr::Report r;
        check(tryResync(f.pso, f.desired(), f.applied(), true, &r) && r.layoutCount == 1 && r.checked == 1 && r.desynced == 0 && r.repaired == 0 && !r.hasFirst,
              "V1.g a healthy list (applied = the wrapper's native buffer): one slot checked, nothing desynced, nothing written");
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
        check(tryResync(f.pso, f.desired(), f.applied(), true, &r) && r.checked == 1 && r.desynced == 0 && f.appliedBuffer(1) == 0 && f.appliedOffset(1) == 77,
              "V1.h a slot at or above the layout's count (slot 1 of a one-slot layout) is not looked at and not written");
    }
    {
        FakeList f;   // a null desired slot is left alone, whatever the applied slot holds
        f.setLayout(2, 20);
        f.setVertexBuffer(1, f.wrapperPtr(1), 0);
        f.flush();
        vr::store64(f.applied() + vr::kAppliedBuffers, kNativeS);   // slot 0 has no desired wrapper but an applied buffer
        vr::store32(f.applied() + vr::kAppliedOffsets, 5);
        vr::Report r;
        check(tryResync(f.pso, f.desired(), f.applied(), true, &r) && r.checked == 1 && r.desynced == 0 && f.appliedBuffer(0) == kNativeS && f.appliedOffset(0) == 5,
              "V1.i a null desired wrapper is not dereferenced, not counted and not written");
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
        check(tryResync(f.pso, f.desired(), f.applied(), true, &r) && r.checked == 3 && r.desynced == 2 && r.repaired == 2 && r.first.slot == 0 && r.first.applied == kNativeS &&
                  f.appliedBuffer(0) == kNativeQ && f.appliedBuffer(1) == kNativeR && f.appliedBuffer(2) == kNativeS && f.appliedOffset(0) == 4 && f.appliedOffset(2) == 12,
              "V1.j three slots, two stale (one NULL, one a different buffer): both repaired to their wrappers' native buffers and desired offsets, the healthy one untouched");
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
        check(tryResync(f.pso, f.desired(), f.applied(), true, &r) && r.layoutCount == 40 && r.checked == 16 && r.desynced == 0 &&
                  std::memcmp(beyondBefore, f.applied() + vr::kAppliedBuffers + 8 * 16, 8) == 0,
              "V1.k a layout that declares more than 16 slots is clamped to 16: nothing past the arrays is read as a wrapper or written");
    }
    {
        vr::Report r;
        FakeList f;
        f.setLayout(1, 20);
        f.setVertexBuffer(0, f.wrapperPtr(0), 0);
        bool ok = tryResync(nullptr, f.desired(), f.applied(), true, &r) && r.checked == 0;
        ok = ok && tryResync(f.pso, nullptr, f.applied(), true, &r) && r.checked == 0;
        ok = ok && tryResync(f.pso, f.desired(), nullptr, true, &r) && r.checked == 0;
        check(ok, "V1.l a null pso, a null desired state and a null applied cache read nothing and write nothing");
        vr::store64(f.pso + vr::kPsoLayout, 0);
        check(tryResync(f.pso, f.desired(), f.applied(), true, &r) && r.layoutCount == 0 && r.checked == 0, "V1.m a pso with no layout object reads nothing further");
        FakeList g;
        g.setLayout(0, 0);
        check(tryResync(g.pso, g.desired(), g.applied(), true, &r) && r.layoutCount == 0 && r.checked == 0 && !r.hasFirst, "V1.n a layout with no slots (the zero-slot draws) checks nothing");
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
void caseKey() {
    check(vr::wantsRepair("on") && vr::wantsRepair("ON") && vr::wantsRepair("") && vr::wantsRepair(nullptr) && vr::wantsRepair("1") && vr::wantsRepair("true") &&
              vr::wantsRepair("auto") && vr::wantsRepair("offf") && vr::wantsRepair("of"),
          "V3.a on is the default: on, any case, empty, missing, and anything that is not a spelling of off");
    check(!vr::wantsRepair("off") && !vr::wantsRepair("OFF") && !vr::wantsRepair("Off") && !vr::wantsRepair("0") && !vr::wantsRepair("false") && !vr::wantsRepair("no"),
          "V3.b off is off, 0, false and no, in any case");
    check(vr::wantsRepair("off off off off off off off off"), "V3.c a value longer than any spelling of off is not off");
    const edvr::RuntimeProfile was = edvr::g_runtimeProfile;
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    check(edvr::runtimeProfileAllowsKey("advanced.vertex_resync"), "V3.d the flat profile lets advanced.vertex_resync through (unlisted, getString would answer off there whatever the file says)");
    check(!edvr::runtimeProfileAllowsKey("advanced.vertex_resync_x") && !edvr::runtimeProfileAllowsKey("advanced.vertex_resy") && !edvr::runtimeProfileAllowsKey("fix.scanner_body"),
          "V3.e ...exactly that key: a longer or shorter name and the VR-only fix stay refused");
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Vr;
    check(edvr::runtimeProfileAllowsKey("advanced.vertex_resync") && edvr::runtimeProfileAllowsKey("fix.scanner_body"), "V3.f the VR profiles allow every key");
    edvr::g_runtimeProfile = was;
}

// ---- V4 ----------------------------------------------------------------------------------------------------------------------------------------------------
void caseInstruments() {
    char line[600];
    vr::formatResyncLine(line, sizeof(line), 7, true);
    check(std::strcmp(line, "vertex resync: 7 stale vertex-buffer bindings in the last 60 s (repaired)") == 0, "V4.a the 60 s line, repair on, is exactly the one asked for");
    vr::formatResyncLine(line, sizeof(line), 7, false);
    check(std::strcmp(line, "vertex resync: 7 stale vertex-buffer bindings in the last 60 s (left as the game had them)") == 0, "V4.b ...and with repair off it says left as the game had them");
    vr::formatLentLine(line, sizeof(line), 3);
    check(std::strcmp(line, "scanner body fix: lent the other eye's buffer 3 times in the last 60 s") == 0, "V4.c the scanner-body running line is exactly the one asked for");
    vr::Sighting s;
    s.slot = 2; s.wrapper = 0xABC0; s.native = 0xDEF0; s.applied = 0x0;
    vr::formatSightingLine(line, sizeof(line), 3, s, 0x7F0000, 1, 4242, true, 2, "0x522A50/0x510404");
    check(has(line, "vertex resync: stale binding 3 of 8: slot 2, list 0x7F0000, desired wrapper 0xABC0, native buffer 0xDEF0, applied buffer 0x0, layout slots 1, thread 4242, repaired; "
                    "game stack (2 frames) 0x522A50/0x510404"),
          "V4.d a first-sighting line carries the slot, the list, the desired wrapper, the native buffer, the applied value, the layout count, the thread and the game stack");
    vr::formatSightingLine(line, sizeof(line), 1, s, 1, 1, 1, false, 0, "");
    check(has(line, "left as the game had it") && has(line, "(none in the game's image)"), "V4.e ...left as the game had it when the repair is off, and the empty stack is said so");

    vr::WindowCount w;
    w.add(5);
    check(w.take(1000) == 0, "V4.f the first tick starts the window and says nothing");
    check(w.take(60999) == 0, "V4.g a tick inside the 60 s says nothing");
    check(w.take(61000) == 5 && w.pending() == 0, "V4.h the tick 60 s after the first says the count and empties it");
    check(w.take(121000) == 0, "V4.i a window with nothing in it says nothing (the line is only for a non-zero count)");
    w.add();
    w.add();
    check(w.take(150000) == 0 && w.take(181000) == 2, "V4.j and the next window with something in it says it, 60 s after the last");
    check(vr::kWindowMs == 60000, "V4.k the window is 60 s");

    vr::SightingGate gate;
    unsigned taken = 0;
    for (int i = 0; i < 20; ++i) taken += gate.take() ? 1u : 0u;
    check(taken == 8 && gate.taken() == 8 && vr::kMaxSightings == 8, "V4.l twenty first sightings are eight lines");
}

// ---- V5: the production hook on a synthetic FlushIA --------------------------------------------------------------------------------------------------------
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
// A stale-slot-0 list to call the synthetic function with.
uintptr_t callFlush(FlushFn fn, FakeList& f) { return fn(reinterpret_cast<uintptr_t>(f.pso), 0, reinterpret_cast<uintptr_t>(f.desired()), reinterpret_cast<uintptr_t>(f.applied())); }
void stale(FakeList& f, uint32_t offset = 0) {
    f.setLayout(1, 20);
    f.setVertexBuffer(0, f.wrapperPtr(0), offset);
    vr::store64(f.applied() + vr::kAppliedBuffers, 0);   // zeroed by a zero-slot flush
    vr::store32(f.applied() + vr::kAppliedOffsets, 0);
}

void caseHook() {
    using namespace edvr;
    const uint8_t exe[16] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x49, 0x8B, 0xF8};
    {
        // V5.a nothing exists before the hook: the synthetic function returns the stale applied[0]
        uint8_t* p = makeSyntheticFlush(exe);
        FakeList f;
        stale(f);
        check(p && callFlush(reinterpret_cast<FlushFn>(p), f) == 0, "V5.a before the hook the synthetic flush sees the stale cache (applied[0] = NULL): the control");
    }
    // V5.b a function whose prologue is not build 332841's is refused: one line, nothing patched, and it is not retried
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
          "V5.b a function whose prologue is not build 332841's is refused: one line, nothing patched, not installed");
    const size_t linesAfterRefusal = vertexResyncTestLineCount();
    vertexResyncInstall();
    check(vertexResyncTestLineCount() == linesAfterRefusal && vertexResyncTestState().attempted, "V5.c a refusal is final: asking again says nothing and tries nothing");

    // V5.d the right function arms (stolen = the first instruction, 5 bytes)
    uint8_t* fn = makeSyntheticFlush(exe);
    g_fn = fn;
    vertexResyncTestReset();
    vertexResyncTestSetTarget(reinterpret_cast<uintptr_t>(fn));
    vertexResyncInstall();
    check(vertexResyncTestState().installed && countLinesWith("vertex resync: hook armed") == 1 && countLinesWith("stolen=5 bytes") == 1 && countLinesWith("prologue 16/16 bytes verified") == 1 &&
              fn[0] == 0xE9,
          "V5.d the right prologue arms the hook: the line names it (stolen = 5 bytes, 16/16 verified) and the entry is a jump");
    const size_t armedLines = vertexResyncTestLineCount();
    vertexResyncInstall();
    check(vertexResyncTestLineCount() == armedLines, "V5.e installing twice changes nothing");

    const FlushFn call = reinterpret_cast<FlushFn>(fn);
    {
        FakeList f;
        stale(f, 24);
        const uintptr_t seen = callFlush(call, f);
        const VertexResyncTestState s = vertexResyncTestState();
        check(seen == kNativeQ && f.appliedBuffer(0) == kNativeQ && f.appliedOffset(0) == 24 && s.calls == 1 && s.desynced == 1 && s.repaired == 1,
              "V5.f the original runs AFTER the repair: with the cache stale, the synthetic flush reads Q's native buffer from applied[0]; the offset was copied; counted 1 desynced, 1 repaired");
        check(countLinesWith("vertex resync: stale binding 1 of 8: slot 0, list 0x") == 1 && countLinesWith("native buffer 0x1000000010, applied buffer 0x0, layout slots 1") == 1 &&
                  countLinesWith("repaired; game stack") == 1,
              "V5.g the first sighting is logged: slot, list, wrapper, native buffer, applied value, layout count, repaired, and the game stack");
        FakeList g;   // a healthy call changes nothing
        g.setLayout(1, 20);
        g.setVertexBuffer(0, g.wrapperPtr(0), 0);
        const size_t lines = vertexResyncTestLineCount();
        check(callFlush(call, g) == kNativeQ && vertexResyncTestState().desynced == 1 && vertexResyncTestState().calls == 2 && vertexResyncTestLineCount() == lines,
              "V5.h a healthy call is forwarded untouched: no desync counted, no line");
    }
    {
        vertexResyncTestSetRepair(false);
        FakeList f;
        stale(f, 24);
        const VertexResyncTestState b = vertexResyncTestState();
        const uintptr_t seen = callFlush(call, f);
        const VertexResyncTestState s = vertexResyncTestState();
        check(seen == 0 && f.appliedBuffer(0) == 0 && f.appliedOffset(0) == 0 && s.desynced == b.desynced + 1 && s.repaired == b.repaired,
              "V5.i with the repair off the original still sees the stale cache, nothing was written, and the desync is counted");
        check(countLinesWith("left as the game had it; game stack") == 1, "V5.j ...and the sighting says it was left as the game had it");
        vertexResyncTestSetRepair(true);
    }
    {
        // twelve more stale calls: eight sightings in all, however many desyncs
        for (int i = 0; i < 12; ++i) {
            FakeList f;
            stale(f);
            callFlush(call, f);
        }
        check(vertexResyncTestState().sightings == 8 && countLinesWith("vertex resync: stale binding ") == 8, "V5.k the first-sighting lines stop at eight");
    }
}

void caseHookPoll() {
    using namespace edvr;
    // The live key and the 60 s line, through the poll (the hook stays in the synthetic function from caseHook; its counters are reset).
    vertexResyncTestReset();
    Config& cfg = Config::get();
    cfg.set("advanced.vertex_resync", "off");
    vertexResyncPoll(cfg, 1000);
    check(!vertexResyncTestState().repair && countLinesWith("advanced.vertex_resync = off") == 1, "V5.o the key read off: the repair is off and the line says so once");
    vertexResyncPoll(cfg, 2000);
    check(countLinesWith("advanced.vertex_resync = off") == 1, "V5.p ...and a poll that changes nothing says nothing more");
    cfg.set("advanced.vertex_resync", "on");
    vertexResyncPoll(cfg, 3000);
    check(vertexResyncTestState().repair && countLinesWith("advanced.vertex_resync = on") == 1, "V5.q turned on live, the repair is on and the line says so");
    check(countLinesWith("stale vertex-buffer bindings in the last 60 s") == 0, "V5.r nothing said about a count while there is none");
    // The 60 s line. The first poll (1000) started the window; seven stale calls through the hooked function, repair on.
    const FlushFn call = reinterpret_cast<FlushFn>(g_fn);
    for (int i = 0; i < 7; ++i) {
        FakeList f;
        stale(f);
        callFlush(call, f);
    }
    vertexResyncPoll(cfg, 60999);
    check(countLinesWith("stale vertex-buffer bindings in the last 60 s") == 0, "V5.s a poll inside the 60 s says nothing about the count");
    vertexResyncPoll(cfg, 61000);
    check(countLinesWith("vertex resync: 7 stale vertex-buffer bindings in the last 60 s (repaired)") == 1, "V5.t the poll 60 s after the first says the count: seven, repaired");
    vertexResyncPoll(cfg, 121000);
    check(countLinesWith("stale vertex-buffer bindings in the last 60 s") == 1, "V5.u a window with nothing in it says nothing: the line is for a non-zero count only");
    cfg.set("advanced.vertex_resync", "off");
    vertexResyncPoll(cfg, 122000);
    for (int i = 0; i < 3; ++i) {
        FakeList f;
        stale(f);
        callFlush(call, f);
    }
    vertexResyncPoll(cfg, 181000);
    check(countLinesWith("vertex resync: 3 stale vertex-buffer bindings in the last 60 s (left as the game had them)") == 1,
          "V5.v with the key off the next window says three, left as the game had them");
    cfg.set("advanced.vertex_resync", "on");
    vertexResyncPoll(cfg, 182000);
}

void caseHookFaults() {
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
              "V5.l a read that faults inside the hook is absorbed and counted, one line, and the game's flush runs as it would have");
        for (int i = 0; i < 10; ++i) {
            FakeList g;
            stale(g);
            vr::store64(g.pso + vr::kPsoLayout, 0x10);
            callFlush(call, g);
        }
        check(vertexResyncTestState().faults == 8 && countLinesWith("stood down after eight faults") == 1,
              "V5.m eight faults stand the hook down (one line) and the relay then forwards straight to the original: the later calls are not even counted");
        FakeList h;
        stale(h);
        check(callFlush(call, h) == 0, "V5.n ...a stale call after the stand-down is not repaired");
    }
}

// ---- V6: the glue, read as text ----------------------------------------------------------------------------------------------------------------------------
void caseGlue(const std::string& root) {
    if (root.empty()) return;
    std::string device, resolve, hook;
    const bool readAll = readText(root + "/src/d3d11/device_hook.cpp", &device) && readText(root + "/src/d3d11/resolve_bind_fix.cpp", &resolve) &&
                         readText(root + "/src/d3d11/vertex_resync_hook.cpp", &hook);
    check(readAll, "V6.p0 device_hook.cpp, resolve_bind_fix.cpp and vertex_resync_hook.cpp can be read under the repository root");
    if (!readAll) return;
    check(occurrences(device, "vertexResyncInstall();") == 1 && at(device, "flatTemporalStart(device);") < at(device, "vertexResyncInstall();") &&
              at(device, "vertexResyncInstall();") < at(device, "armFlipTimeline(sentinelCfg, table, span, \"context\");"),
          "V6.a the hook is installed once, at startup, after the other installers and in either profile (no profile test around it)");
    check(occurrences(device, "vertexResyncPoll(Config::get(), stampMs());") == 1 && occurrences(device, "resolveBindTick(stampMs());") == 1 &&
              at(device, "vScreenRefreshConfig();") < at(device, "vertexResyncPoll(Config::get(), stampMs());") &&
              at(device, "vertexResyncPoll(Config::get(), stampMs());") < at(device, "resolveBindTick(stampMs());"),
          "V6.b the live key and the two running counts are asked once a second, in the config tick, after the reload");
    check(occurrences(hook, "cfg.getString(\"advanced.vertex_resync\", \"on\")") == 1 && occurrences(hook, "vresync::wantsRepair(value.c_str())") == 1,
          "V6.c the hook reads advanced.vertex_resync with default on, through the key parser");
    check(occurrences(resolve, "++g_healed;\n        g_lentWindow.add();") == 1 && occurrences(resolve, "scanner body fix: ENGAGED") == 1 &&
              occurrences(resolve, "vresync::formatLentLine(line, sizeof(line), n);") == 1 && occurrences(resolve, "uint32_t resolveBindTick(uint64_t nowMs) {") == 1,
          "V6.d every lend counts in the running window, the first ENGAGED line is kept, and the tick says the count through the shared formatter");
    check(occurrences(hook, "if (!r.hasFirst || !g_sightings.take()) return;") == 1 && occurrences(hook, "const bool repair = g_repair.load(std::memory_order_relaxed);") == 1 &&
              occurrences(hook, "return forward(pso, context, desired, applied);") == 1,
          "V6.e the callback honours the key per call, caps the sightings, and forwards to the original");
}
}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false;
    std::string root;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--self-test" || a == "--dry-run") selfTest = true;
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
    caseKey();
    caseInstruments();
    caseHook();
    caseHookPoll();
    caseHookFaults();
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
