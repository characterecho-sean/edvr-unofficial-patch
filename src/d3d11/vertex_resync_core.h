#pragma once
// The game's stale vertex-buffer binding, and the one-line repair at the entry of the function that applies it (docs/scanner-body.md, "The root cause: Frontier's
// f3d state cache"). Everything here is pure -- raw pointers, bytes and numbers, no Windows -- so a rig can drive it on a synthetic list laid out at the real offsets
// (tools\vertex_resync_test). vertex_resync_hook.cpp is the glue that finds the function, patches it, and calls this under SEH.
//
// THE MECHANISM, as read from build 332841 (analysis\decomp\scanner_vb_dump_*; READ = a disassembled instruction, INFERRED = the sequence it implies):
//
//   Elite's D3D11 goes through Frontier's "f3d" layer. A command list (the immediate context's, or a deferred one) holds two copies of the input-assembler vertex-buffer
//   state, at fixed offsets from the list:
//     DESIRED   list+0x60 + 8*slot  wrapper (an f3d buffer object, refcounted)      list+0xE0 + 4*slot  offset      list+0x120 + 4*slot  stride override
//     APPLIED   list+0x408 + 8*slot native ID3D11Buffer*   list+0x488 + 4*slot stride   list+0x4C8 + 4*slot offset            (the cache of what was last BOUND)
//
//   SetVertexBuffer (0x1404E9D70; READ) compares ONLY the desired state -- [list+8*slot+0x60], [list+4*slot+0xE0], [list+4*slot+0x120] against the arguments -- and
//   returns at 0x1404E9DAF when they are equal. When they differ it writes the desired state and the applied cache together: applied buffer = [wrapper+0x140] (the
//   native buffer), applied offset = the offset, applied stride = 0, and sets bit 0x20 of the dirty word at list+0x1E8.
//
//   FlushIA (0x140522A50; READ), called before a draw with rcx = the pipeline state, r8 = list+0x60 (DESIRED), r9 = list+0x2A0 (so r9+0x168 = APPLIED buffers,
//   r9+0x228 = offsets, r9+0x1E8 = strides), computes each slot's stride from the desired state, and when the draw's layout has FEWER slots than the previous draw's it
//   zeroes applied buffer, offset and stride for the slots from the new count up to the old (0x140522B7C..0x140522BA0). It never touches the desired state. It then
//   binds from the APPLIED arrays (the call at 0x140522BF5 is IASetVertexBuffers with [r9+0x168], strides, offsets).
//
//   The failing sequence (INFERRED from those, and consistent with 38 of 38 field frames): the first eye's deferred-lighting resolve binds buffer Q (desired = applied
//   = Q); two zero-slot full-screen draws follow, whose FlushIA zeroes APPLIED[0] and binds (NULL, 0, 0) while DESIRED[0] stays Q; the second eye's resolve calls
//   SetVertexBuffer(Q), which equals DESIRED and is SKIPPED, so APPLIED[0] is not rewritten; FlushIA computes stride 20 from DESIRED and binds APPLIED[0] = NULL:
//   (NULL, 20, 0). The vertex shader reads real attributes, the quad is degenerate, nothing rasterises, and the scanned body stays black in that eye.
//
//   The repair: at FlushIA's entry, for every slot the draw's layout uses (count = [[pso+0x188]+0x60], at most 16), where DESIRED holds a wrapper, make APPLIED agree
//   with it -- applied buffer = [wrapper+0x140], applied offset = the desired offset. Slots at or above the count, empty desired slots and every stride are left as the
//   game has them, so stock behaviour is otherwise identical. fix.scanner_body (resolve_bind_fix.cpp) healed the same symptom one level up, by lending a buffer to the
//   resolve draw; that is retired after this has flown.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace vresync {

// ---- identity: build 332841 (EliteDangerous64.exe, SHA-256 e6be8bbe...4e988), the pair every build-keyed hook here checks --------------------------------------
constexpr uint32_t kExpectedTimestamp = 1788384820u;
constexpr uint32_t kExpectedImageSize = 104894464u;
constexpr uintptr_t kFlushIaRva = 0x522A50u;   // 0x140522A50
// The first 16 bytes of FlushIA, read from the exe (Epic and the analysis copy agree): mov [rsp+18h],rbx / push rsi / push rdi / push r14 / sub rsp,30h / mov rdi,r8.
// CodeHook steals the first instruction (5 bytes, no relative operand) and nothing else.
constexpr size_t kPrologueBytes = 16;
constexpr uint8_t kFlushIaPrologue[kPrologueBytes] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x49, 0x8B, 0xF8};

inline bool identityMatches(uint32_t timestamp, uint32_t imageSize) { return timestamp == kExpectedTimestamp && imageSize == kExpectedImageSize; }
inline bool prologueMatches(const uint8_t* bytes, size_t n) { return bytes && n >= kPrologueBytes && std::memcmp(bytes, kFlushIaPrologue, kPrologueBytes) == 0; }

// ---- the f3d layout, as offsets from the three pointers FlushIA is called with --------------------------------------------------------------------------------
constexpr uint32_t kMaxSlots = 16;           // the arrays below hold sixteen slots each (the desired offsets start 0x80 bytes after the desired wrappers)
constexpr size_t kPsoLayout = 0x188;         // pso -> the input-layout object ([rsi+0x188] at 0x140522AAB)
constexpr size_t kLayoutSlots = 0x60;        // layout -> dword: the slots the layout declares ([rax+0x60] at 0x140522B1E)
constexpr size_t kWrapperNative = 0x140;     // wrapper -> the native ID3D11Buffer* (SetVertexBuffer: [rax+0x140] at 0x1404E9E20)
constexpr size_t kDesiredWrappers = 0;       // desired + 8*slot
constexpr size_t kDesiredOffsets = 0x80;     // desired + 0x80 + 4*slot  (list+0xE0)
constexpr size_t kAppliedBuffers = 0x168;    // applied + 0x168 + 8*slot (list+0x408)
constexpr size_t kAppliedOffsets = 0x228;    // applied + 0x228 + 4*slot (list+0x4C8)
constexpr size_t kListDesired = 0x60;        // list -> desired
constexpr size_t kListApplied = 0x2A0;       // list -> applied

inline uint64_t load64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline uint32_t load32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline void store64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }
inline void store32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

struct Sighting {
    uint32_t slot = 0;
    uint64_t wrapper = 0, native = 0, applied = 0;
};
struct Report {
    uint32_t layoutCount = 0;   // the slots the draw's layout declares (as read; the loop is bounded by kMaxSlots)
    uint32_t checked = 0;       // slots with a desired wrapper
    uint32_t desynced = 0;      // ...whose applied buffer is not that wrapper's native buffer
    uint32_t repaired = 0;      // ...and which were rewritten (repair on)
    bool hasFirst = false;
    Sighting first;             // the first desynced slot
};

// FlushIA's entry. `pso` is rcx, `desired` r8, `applied` r9. Every pointer read is guarded against null: a null pso, desired or applied, a null layout object and a null wrapper
// read nothing further. The caller (the hook) runs this under SEH, because the pointers it follows are the game's. With `repair` false nothing is written (count only).
inline Report resync(const uint8_t* pso, const uint8_t* desired, uint8_t* applied, bool repair) {
    Report r;
    if (!pso || !desired || !applied) return r;
    const uint64_t layoutAddress = load64(pso + kPsoLayout);
    if (!layoutAddress) return r;
    r.layoutCount = load32(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(layoutAddress)) + kLayoutSlots);
    const uint32_t slots = r.layoutCount < kMaxSlots ? r.layoutCount : kMaxSlots;
    for (uint32_t i = 0; i < slots; ++i) {
        const uint64_t wrapper = load64(desired + kDesiredWrappers + 8u * i);
        if (!wrapper) continue;
        const uint64_t native = load64(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(wrapper)) + kWrapperNative);
        uint8_t* appliedBuffer = applied + kAppliedBuffers + 8u * i;
        const uint64_t was = load64(appliedBuffer);
        ++r.checked;
        if (was == native) continue;
        ++r.desynced;
        if (!r.hasFirst) {
            r.hasFirst = true;
            r.first.slot = i;
            r.first.wrapper = wrapper;
            r.first.native = native;
            r.first.applied = was;
        }
        if (repair) {
            store64(appliedBuffer, native);
            store32(applied + kAppliedOffsets + 4u * i, load32(desired + kDesiredOffsets + 4u * i));
            ++r.repaired;
        }
    }
    return r;
}

// ---- the key ---------------------------------------------------------------------------------------------------------------------------------------------------
// advanced.vertex_resync = on | off (default on, live). Off counts and writes nothing. TEMPORARY: it exists for the scanner-body A/B flight (docs/scanner-body.md), and goes
// when the arc closes.
inline bool wantsRepair(const char* text) {
    if (!text) return true;
    char lower[16] = {};
    size_t n = 0;
    for (; text[n] && n < sizeof(lower) - 1; ++n) lower[n] = static_cast<char>(text[n] >= 'A' && text[n] <= 'Z' ? text[n] + 32 : text[n]);
    return !(std::strcmp(lower, "off") == 0 || std::strcmp(lower, "0") == 0 || std::strcmp(lower, "false") == 0 || std::strcmp(lower, "no") == 0);
}

// ---- the instruments -------------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kMaxSightings = 8;          // first-sighting lines, ever
constexpr uint64_t kWindowMs = 60000;          // the running counts are said once per this, while non-zero

// A count said once per window while it is non-zero (the lend count, the resync count). Many threads add; the tick takes. The first tick starts the window.
class WindowCount {
public:
    void add(uint32_t n = 1) { count_.fetch_add(n, std::memory_order_relaxed); }
    // The count of the window that ended, or 0 when the window is still running or nothing happened in it. A window that ends restarts, empty or not.
    uint32_t take(uint64_t nowMs) {
        if (!started_) {
            started_ = true;
            startMs_ = nowMs;
            return 0;
        }
        if (nowMs - startMs_ < kWindowMs) return 0;
        startMs_ = nowMs;
        return count_.exchange(0, std::memory_order_relaxed);
    }
    uint32_t pending() const { return count_.load(std::memory_order_relaxed); }
    void reset() { count_.store(0, std::memory_order_relaxed); started_ = false; startMs_ = 0; }

private:
    std::atomic<uint32_t> count_{0};
    bool started_ = false;   // the tick's own (one thread)
    uint64_t startMs_ = 0;
};

// At most kMaxSightings first-sighting lines: true for the first eight askers.
class SightingGate {
public:
    bool take() { return taken_.fetch_add(1, std::memory_order_relaxed) < kMaxSightings; }
    uint32_t taken() const { const uint32_t n = taken_.load(std::memory_order_relaxed); return n < kMaxSightings ? n : kMaxSightings; }
    void reset() { taken_.store(0, std::memory_order_relaxed); }

private:
    std::atomic<uint32_t> taken_{0};
};

inline void formatResyncLine(char* out, size_t cap, uint32_t n, bool repaired) {
    std::snprintf(out, cap, "vertex resync: %u stale vertex-buffer bindings in the last 60 s (%s)", n, repaired ? "repaired" : "left as the game had them");
}
inline void formatLentLine(char* out, size_t cap, uint32_t n) {
    std::snprintf(out, cap, "scanner body fix: lent the other eye's buffer %u times in the last 60 s", n);
}
inline void formatSightingLine(char* out, size_t cap, uint32_t number, const Sighting& s, uint64_t list, uint32_t layoutCount, uint32_t threadId, bool repaired,
                               unsigned gameFrames, const char* stack) {
    std::snprintf(out, cap,
                  "vertex resync: stale binding %u of %u: slot %u, list 0x%llX, desired wrapper 0x%llX, native buffer 0x%llX, applied buffer 0x%llX, layout slots %u, thread %u, %s; "
                  "game stack (%u frames) %s",
                  number, kMaxSightings, s.slot, static_cast<unsigned long long>(list), static_cast<unsigned long long>(s.wrapper), static_cast<unsigned long long>(s.native),
                  static_cast<unsigned long long>(s.applied), layoutCount, threadId, repaired ? "repaired" : "left as the game had it", gameFrames, stack && *stack ? stack : "(none in the game's image)");
}

}  // namespace vresync
}  // namespace edvr
