#include "skin_entity_hook.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../common/code_hook.h"
#include "explorer_cam_core.h"
#include "skin_entity_walk.h"

namespace edvr {
namespace {

using namespace skinjoin;

constexpr uintptr_t kSkinJobsRva = 0x4C540E0;
constexpr size_t kPrologueBytes = 28;
// `mov rax,rsp; push rbp; push rbx; push r13; lea rbp,[rax-5Fh]; sub rsp,0C0h; mov r13,[rcx+0A8h]; mov rbx,rcx`, read from the build-332841
// executable with the PE headers (timestamp 1788384820, image 104894464). No rip-relative byte; the boundaries are 3, 4, 5, 7, 11, 18, 25, 28,
// so CodeHook steals 5 (`mov rax,rsp`, `push rbp`, `push rbx`) and the trampoline re-runs them.
constexpr uint8_t kPrologue[kPrologueBytes] = {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x41, 0x55, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC,
                                               0xC0, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xA9, 0xA8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD9};
constexpr uint64_t kStandDownAfter = 120;   // lists that were judged (made with something to skin) with none usable

// The function takes one argument (the node, rcx); the other register arguments are passed on untouched so a caller that leaves something
// in them sees the original behave exactly as it did.
using JobsFn = void(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

struct State {
    CodeHook hook;
    uint8_t* relay = nullptr;
    uintptr_t target = 0;
    alignas(8) std::atomic<uintptr_t> gate{0};      // the relay compares this qword with zero
    std::atomic<uintptr_t> forward{0};
    std::atomic<int> state{int(SkinHookState::NotTried)};
    char why[320] = {};
    // the ring: four snapshots, a sequence number per slot (0 while being written)
    Snapshot ring[4];
    std::atomic<uint64_t> slotSeq[4]{};
    std::atomic<uint64_t> latest{0};
    std::atomic<uint64_t> calls{0}, usable{0}, faulted{0}, overflowed{0}, implausible{0}, otherUnusable{0}, nodeChanges{0};
    // The stand-down judges a list only against a job table that has jobs (2026-10-08, the F12 flight: 120 empty lists made on the main menu stood the
    // hook down before a character existed). A clean empty list is the assembler having nobody to skin: it is counted (emptyLists) and judged by
    // nothing here; noteChain() judges it when a chain dispatch with jobs finds it the newest list (emptyWithJobs). `judged` counts every other list.
    std::atomic<uint64_t> judged{0}, emptyLists{0}, emptyWithJobs{0};
    std::atomic<uintptr_t> firstNode{0};
    std::atomic<uint32_t> firstTid{0}, lastTid{0}, lastEntries{0}, lastEnd{0}, lastFlags{0};
    std::atomic<uint32_t> tids[4]{};
    std::atomic<uint32_t> threads{0};
    std::mutex events;
    std::vector<std::string> queue;
};
std::atomic<State*> g_state{nullptr};
std::mutex g_armMutex;

#ifdef EDVR_SKIN_HOOK_TEST
uintptr_t g_testTarget = 0;
void (*g_testMidRead)() = nullptr;   // called once, between a reader's copy and its verification: the rig laps the writer there
#endif

void postEvent(State& s, const char* line) {
    std::lock_guard<std::mutex> lock(s.events);
    if (s.queue.size() < 16) s.queue.emplace_back(line);
}

// The only place game memory is read. A fault returns false and nothing else.
__declspec(noinline) bool guardedRead(uintptr_t address, void* out, size_t bytes) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
struct Reader {
    bool operator()(uint64_t address, void* out, size_t bytes) const noexcept { return guardedRead(static_cast<uintptr_t>(address), out, bytes); }
};

__declspec(noinline) bool checkBytes(uintptr_t address, const uint8_t* expected, size_t n) noexcept {
    __try {
        return std::memcmp(reinterpret_cast<const void*>(address), expected, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool checkIdentity(uintptr_t base, const char** why) noexcept {
    __try {
        uint32_t peOff = 0;
        std::memcpy(&peOff, reinterpret_cast<const void*>(base + 0x3C), 4);
        if (peOff > 0x1000) { *why = "the PE header offset is implausible"; return false; }
        uint32_t timestamp = 0, imageSize = 0;
        std::memcpy(&timestamp, reinterpret_cast<const void*>(base + peOff + 8), 4);
        std::memcpy(&imageSize, reinterpret_cast<const void*>(base + peOff + 0x50), 4);
        if (timestamp != ecm::kExpectedTimestamp || imageSize != ecm::kExpectedImageSize) {
            *why = "the PE timestamp or image size is not build 332841's";
            return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "a read faulted while checking the PE header";
        return false;
    }
}

// A list of no entries and a plain end row (the first free row, 1): the assembler had nobody to skin (a menu, a loading screen). It says nothing
// about the offsets by itself: an empty list with a nonzero end row, or any walk flag, is not this and is judged.
bool emptyList(const Snapshot& s) noexcept { return s.n == 0 && s.flags == 0 && s.end <= 1; }

void standDown(State& s, const char* why) {
    s.gate.store(0, std::memory_order_seq_cst);
    s.latest.store(0, std::memory_order_release);
    int expected = int(SkinHookState::Armed);
    if (s.state.compare_exchange_strong(expected, int(SkinHookState::StoodDown))) {
        std::snprintf(s.why, sizeof(s.why), "%s", why);
        char line[400];
        std::snprintf(line, sizeof(line), "skin join: the hook stood down: %s. The job table's prefix join takes over; the game was not touched.", why);
        postEvent(s, line);
    }
}

void observe(State& s, uintptr_t node) noexcept {
    const uint64_t seq = s.calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const size_t slotIndex = size_t(seq & 3);
    Snapshot& slot = s.ring[slotIndex];
    s.slotSeq[slotIndex].exchange(0, std::memory_order_seq_cst);   // a full barrier: the writes below cannot move above it
    walkNode(Reader{}, node, slot);
    slot.seq = seq;
    slot.tid = GetCurrentThreadId();
    uintptr_t first = s.firstNode.load(std::memory_order_relaxed);
    if (!first) {
        s.firstNode.compare_exchange_strong(first, node);
        first = s.firstNode.load(std::memory_order_relaxed);
    }
    bool second = false;
    if (first != node) {
        slot.flags |= kSnapNodeChanged;
        second = s.nodeChanges.fetch_add(1, std::memory_order_relaxed) == 0;
    }
    const char* why = "";
    const bool good = checkSnapshot(slot, &why);
    const bool empty = !good && emptyList(slot);
    if (good) s.usable.fetch_add(1, std::memory_order_relaxed);
    else if (empty) s.emptyLists.fetch_add(1, std::memory_order_relaxed);
    else if (slot.flags & kSnapFault) s.faulted.fetch_add(1, std::memory_order_relaxed);
    else if (slot.flags & kSnapOverflow) s.overflowed.fetch_add(1, std::memory_order_relaxed);
    else if (slot.flags & kSnapImplausible) s.implausible.fetch_add(1, std::memory_order_relaxed);
    else s.otherUnusable.fetch_add(1, std::memory_order_relaxed);
    const uint64_t judged = empty ? s.judged.load(std::memory_order_relaxed) : s.judged.fetch_add(1, std::memory_order_relaxed) + 1;
    s.lastEntries.store(slot.n, std::memory_order_relaxed);
    s.lastEnd.store(slot.end, std::memory_order_relaxed);
    s.lastFlags.store(slot.flags, std::memory_order_relaxed);
    // threads
    const uint32_t tid = slot.tid;
    s.lastTid.store(tid, std::memory_order_relaxed);
    uint32_t known = s.threads.load(std::memory_order_relaxed);
    bool seen = false;
    for (uint32_t i = 0; i < known && i < 4; ++i) seen = seen || s.tids[i].load(std::memory_order_relaxed) == tid;
    if (!seen && known < 4) {
        s.tids[known].store(tid, std::memory_order_relaxed);
        s.threads.store(known + 1, std::memory_order_relaxed);
        char line[400];
        std::snprintf(line, sizeof(line),
                      "skin join: hook %s on thread %u: node 0x%llX, %u entries, end row %u, bases %s (%s)",
                      known == 0 ? "first call" : "call from another thread", tid, static_cast<unsigned long long>(node), slot.n, slot.end,
                      slot.n ? "read" : "none", good ? "the list is consistent" : why);
        postEvent(s, line);
        if (known == 0) s.firstTid.store(tid, std::memory_order_relaxed);
    }
    s.slotSeq[slotIndex].store(seq, std::memory_order_release);
    s.latest.store(seq, std::memory_order_release);
    if (second) {
        char line[300];
        std::snprintf(line, sizeof(line), "a second processor node (0x%llX, the first was 0x%llX): the hook cannot say which one a dispatch belongs to",
                      static_cast<unsigned long long>(node), static_cast<unsigned long long>(first));
        standDown(s, line);
    } else if (judged >= kStandDownAfter && s.usable.load(std::memory_order_relaxed) == 0) {
        char line[300];
        std::snprintf(line, sizeof(line), "none of the first %llu lists with something to read was usable (the last: %s); the decompile's offsets do not describe this build's list",
                      static_cast<unsigned long long>(judged), why);
        standDown(s, line);
    }
}

// One chain dispatch (the consumer, once a frame, before it reads the newest list). The dispatch having jobs is what makes an empty list evidence: the
// assembler has just built a table with jobs in it and the list the hook read from the same call names nobody. Dispatches with no jobs judge nothing.
void noteChain(State& s, uint32_t jobs) noexcept {
    if (!jobs || s.state.load(std::memory_order_relaxed) != int(SkinHookState::Armed)) return;
    if (!s.latest.load(std::memory_order_acquire)) return;
    if (s.lastEntries.load(std::memory_order_relaxed) != 0 || s.lastFlags.load(std::memory_order_relaxed) != 0 || s.lastEnd.load(std::memory_order_relaxed) > 1) return;
    const uint64_t n = s.emptyWithJobs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n >= kStandDownAfter && s.usable.load(std::memory_order_relaxed) == 0) {
        char line[300];
        std::snprintf(line, sizeof(line), "the job table had jobs on %llu dispatches and the hook's list had no entries each time; the decompile's offsets do not describe this build's list",
                      static_cast<unsigned long long>(n));
        standDown(s, line);
    }
}

// The original FIRST (its prologue is the trampoline's), then the read. The relay jumps here, so the return address is the game's caller.
__declspec(noinline) void __fastcall skinJobsHooked(uintptr_t node, uintptr_t b, uintptr_t c, uintptr_t d) noexcept {
    State* s = g_state.load(std::memory_order_acquire);
    if (!s) return;
    const auto forward = reinterpret_cast<JobsFn>(s->forward.load(std::memory_order_acquire));
    if (!forward) return;
    forward(node, b, c, d);
    if (s->gate.load(std::memory_order_relaxed)) observe(*s, node);
}

uint8_t* allocateRelay(uintptr_t target) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const uintptr_t granularity = info.dwAllocationGranularity;
    const uintptr_t floor = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t ceiling = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    const uintptr_t distance = uintptr_t(INT32_MAX) - 0x10000u;
    uintptr_t at = target > distance ? target - distance : floor;
    if (at < floor) at = floor;
    const uintptr_t limit = target > ceiling - distance ? ceiling : target + distance;
    while (at < limit) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof(region))) break;
        const uintptr_t start = reinterpret_cast<uintptr_t>(region.BaseAddress);
        if (region.RegionSize > UINTPTR_MAX - start) break;
        const uintptr_t end = start + region.RegionSize;
        if (region.State == MEM_FREE) {
            uintptr_t candidate = at > start ? at : start;
            if (candidate > UINTPTR_MAX - (granularity - 1)) break;
            candidate = (candidate + granularity - 1) & ~(granularity - 1);
            if (candidate < limit && candidate < end && end - candidate >= 4096) {
                auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (p) return p;
            }
        }
        if (end <= at) break;
        at = end;
    }
    return nullptr;
}

bool prepareRelay(void* trampoline, void* context) noexcept {
    auto& s = *static_cast<State*>(context);
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(s.relay + ecm::kCallbackRelayTrampolineAt, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(s.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) || !FlushInstructionCache(GetCurrentProcess(), s.relay, ecm::kCallbackRelayBytes)) return false;
    s.forward.store(address, std::memory_order_release);
    return true;
}

}  // namespace

SkinHookState skinEntityHookArm(char* whyOut, size_t cap) {
    std::lock_guard<std::mutex> lock(g_armMutex);
    State* s = g_state.load(std::memory_order_acquire);
    if (s && s->state.load() != int(SkinHookState::NotTried)) {
        if (whyOut && cap) std::snprintf(whyOut, cap, "%s", s->why);
        return SkinHookState(s->state.load());
    }
    if (!s) {
        s = new State;   // process lifetime: a call already inside the relay must be able to finish
        g_state.store(s, std::memory_order_release);
    }
    const auto stood = [&](const char* why) {
        s->state.store(int(SkinHookState::StoodDown));
        std::snprintf(s->why, sizeof(s->why), "the hook stood down: %s. The job table's prefix join is used; nothing was patched.", why);
        if (whyOut && cap) std::snprintf(whyOut, cap, "%s", s->why);
        return SkinHookState::StoodDown;
    };
    uintptr_t target = 0;
#ifdef EDVR_SKIN_HOOK_TEST
    target = g_testTarget;
#endif
    if (!target) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!base) return stood("the game module could not be resolved");
        const char* peWhy = nullptr;
        if (!checkIdentity(base, &peWhy)) {
            char b[200];
            std::snprintf(b, sizeof(b), "the game build differs (%s); EliteDangerous64.exe+0x%llX was not touched", peWhy ? peWhy : "?", static_cast<unsigned long long>(kSkinJobsRva));
            return stood(b);
        }
        target = base + kSkinJobsRva;
    }
    if (!checkBytes(target, kPrologue, kPrologueBytes)) {
        char b[260];
        std::snprintf(b, sizeof(b), "the game build differs (the %zu bytes at EliteDangerous64.exe+0x%llX are not build 332841's job-assembly prologue)", kPrologueBytes, static_cast<unsigned long long>(kSkinJobsRva));
        return stood(b);
    }
    s->target = target;
    s->relay = allocateRelay(target);
    if (!s->relay) return stood("no executable memory could be placed within two gigabytes of the target");
    ecm::buildCallbackRelay(s->relay, &s->gate, reinterpret_cast<const void*>(&skinJobsHooked));
    // the gate opens only after the patch is live and the forward pointer is published
    if (!s->hook.install(reinterpret_cast<void*>(target), s->relay, nullptr, "skin-jobs", &prepareRelay, s)) {
        VirtualFree(s->relay, 0, MEM_RELEASE);
        s->relay = nullptr;
        return stood("CodeHook refused it (its own line above, tagged skin-jobs, names why)");
    }
    s->state.store(int(SkinHookState::Armed));
    s->gate.store(1, std::memory_order_seq_cst);
    char b[400];
    std::snprintf(b, sizeof(b),
                  "skin join: hook armed: EliteDangerous64.exe+0x%llX (the skinning job assembly, build 332841) at 0x%llX, stolen=%zu bytes, prologue %zu/%zu bytes verified, relay at 0x%llX; "
                  "READ ONLY: the original runs first, then the entry list is read under SEH, nothing is written in game memory",
                  static_cast<unsigned long long>(kSkinJobsRva), static_cast<unsigned long long>(target), s->hook.stolenBytes(), kPrologueBytes, kPrologueBytes,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s->relay)));
    std::snprintf(s->why, sizeof(s->why), "%s", b);
    if (whyOut && cap) std::snprintf(whyOut, cap, "%s", b);
    return SkinHookState::Armed;
}

SkinHookState skinEntityHookState() {
    State* s = g_state.load(std::memory_order_acquire);
    return s ? SkinHookState(s->state.load()) : SkinHookState::NotTried;
}

void skinEntityHookSetGate(bool open) {
    State* s = g_state.load(std::memory_order_acquire);
    if (!s || s->state.load() != int(SkinHookState::Armed)) return;
    s->gate.store(open ? 1 : 0, std::memory_order_seq_cst);
}

bool skinEntityHookLatest(skinjoin::Snapshot& out) {
    State* s = g_state.load(std::memory_order_acquire);
    if (!s || s->state.load() != int(SkinHookState::Armed)) return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint64_t seq = s->latest.load(std::memory_order_acquire);
        if (!seq) return false;
        const size_t slot = size_t(seq & 3);
        if (s->slotSeq[slot].load(std::memory_order_acquire) != seq) continue;
        std::memcpy(&out, &s->ring[slot], sizeof(Snapshot));
        std::atomic_thread_fence(std::memory_order_acquire);
#ifdef EDVR_SKIN_HOOK_TEST
        if (g_testMidRead) {
            void (*seam)() = g_testMidRead;
            g_testMidRead = nullptr;
            seam();
        }
#endif
        if (s->slotSeq[slot].load(std::memory_order_acquire) == seq && out.seq == seq) return true;
    }
    return false;
}

void skinEntityHookNoteChain(uint32_t jobs) {
    State* s = g_state.load(std::memory_order_acquire);
    if (s) noteChain(*s, jobs);
}

SkinHookStats skinEntityHookStats() {
    SkinHookStats r;
    State* s = g_state.load(std::memory_order_acquire);
    if (!s) return r;
    r.state = SkinHookState(s->state.load());
    r.calls = s->calls.load();
    r.usable = s->usable.load();
    r.faulted = s->faulted.load();
    r.overflowed = s->overflowed.load();
    r.implausible = s->implausible.load();
    r.otherUnusable = s->otherUnusable.load();
    r.nodeChanges = s->nodeChanges.load();
    r.judged = s->judged.load();
    r.emptyLists = s->emptyLists.load();
    r.emptyWithJobs = s->emptyWithJobs.load();
    r.firstTid = s->firstTid.load();
    r.lastTid = s->lastTid.load();
    r.threads = s->threads.load();
    r.lastEntries = s->lastEntries.load();
    r.lastEnd = s->lastEnd.load();
    std::snprintf(r.why, sizeof(r.why), "%s", s->why);
    return r;
}

bool skinEntityHookNextEvent(char* line, size_t cap) {
    State* s = g_state.load(std::memory_order_acquire);
    if (!s || !line || !cap) return false;
    std::lock_guard<std::mutex> lock(s->events);
    if (s->queue.empty()) return false;
    std::snprintf(line, cap, "%s", s->queue.front().c_str());
    s->queue.erase(s->queue.begin());
    return true;
}

#ifdef EDVR_SKIN_HOOK_TEST
// ---- the self-test: a synthetic game function with the real prologue, a fake heap laid out as the decompile says ------------------------
namespace {
struct Heap {
    uint8_t* base = nullptr;
    size_t size = 1 << 20, used = 0;
    uint64_t alloc(size_t n) { used = (used + 15) & ~size_t(15); const uint64_t at = reinterpret_cast<uint64_t>(base + used); used += n; return at; }
};
void put64(uint64_t at, uint64_t v) { std::memcpy(reinterpret_cast<void*>(at), &v, 8); }
void put32(uint64_t at, uint32_t v) { std::memcpy(reinterpret_cast<void*>(at), &v, 4); }
void put16(uint64_t at, uint16_t v) { std::memcpy(reinterpret_cast<void*>(at), &v, 2); }
// The seam lapTheWriter: four more calls of the game's function, so the slot a reader is copying is rewritten.
uint64_t s_lapNode = 0;
void (__fastcall* s_lapCall)(uintptr_t, uintptr_t, uintptr_t, uintptr_t) = nullptr;
void lapTheWriter() {
    for (int i = 0; i < 4; ++i) s_lapCall(uintptr_t(s_lapNode), 0, 0, 0);
}
// Lays out a node and `n` entries; entry i has bone count counts[i] and gets dst running from `first`.
uint64_t layout(Heap& h, uint32_t n, const uint32_t* counts, uint32_t first, std::vector<uint64_t>& entries, uint32_t stamp = 0) {
    const uint64_t node = h.alloc(0x200);
    std::memset(reinterpret_cast<void*>(node), 0, 0x200);
    entries.clear();
    uint32_t dst = first;
    uint64_t previous = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint64_t e = h.alloc(0x100), mesh = h.alloc(0x50);
        std::memset(reinterpret_cast<void*>(e), 0, 0x100);
        put64(e, 0x140000000ull + 0x1000 + (i & 1));
        put64(e + 0x38, mesh);
        put16(mesh, uint16_t(counts[i] + stamp));
        put32(e + 0xA8, dst);
        dst += counts[i] + stamp;
        if (previous) put64(previous + 8, e); else put64(node + 0xA8, e);
        previous = e;
        entries.push_back(e);
    }
    put32(node + 0xC4, dst);
    return node;
}
}  // namespace

unsigned skinEntityHookSelfTest(char* detail, size_t cap) {
    unsigned failures = 0;
    const auto fail = [&](const char* id, const char* what) {
        ++failures;
        if (detail && cap) {
            const size_t used = std::strlen(detail);
            if (used + 1 < cap) std::snprintf(detail + used, cap - used, "FAIL: %s -- %s\n", id, what);
        }
    };
    if (detail && cap) detail[0] = 0;
    Heap heap;
    heap.base = static_cast<uint8_t*>(VirtualAlloc(nullptr, heap.size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!heap.base || !code) return 1;
    // the real prologue, then: mov dword ptr [rcx+0C4h], 4Dh (the "original ran" witness), the epilogue
    const uint8_t body[] = {0xC7, 0x81, 0xC4, 0x00, 0x00, 0x00, 0x4D, 0x00, 0x00, 0x00, 0x48, 0x81, 0xC4, 0xC0, 0x00, 0x00, 0x00, 0x41, 0x5D, 0x5B, 0x5D, 0xC3};
    std::memcpy(code, kPrologue, kPrologueBytes);
    std::memcpy(code + kPrologueBytes, body, sizeof(body));
    DWORD old = 0;
    VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), code, 4096);
    g_testTarget = reinterpret_cast<uintptr_t>(code);
    auto call = reinterpret_cast<void(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t)>(code);
    std::vector<uint64_t> entries;
    const uint32_t counts[3] = {40, 50, 30};
    const uint64_t node = layout(heap, 3, counts, 1, entries);
    put32(node + 0xC4, 0);   // the original must be what fills it in
    char text[400];
    // before arming: the call is the game's alone
    call(node, 0, 0, 0);
    if (g_state.load() != nullptr) fail("H1.a", "a state exists before arming");
    skinjoin::Snapshot snap;
    if (skinEntityHookLatest(snap)) fail("H1.b", "a snapshot before arming");
    // a build that is not 332841 stands down: the test target bypasses the PE check, so check the prologue refusal instead
    {
        auto* wrong = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        std::memcpy(wrong, kPrologue, kPrologueBytes);
        wrong[5] ^= 0xFF;
        g_testTarget = reinterpret_cast<uintptr_t>(wrong);
        const SkinHookState r = skinEntityHookArm(text, sizeof(text));
        if (r != SkinHookState::StoodDown || !std::strstr(text, "prologue")) fail("H2.a", "a wrong prologue did not stand the hook down");
        delete g_state.exchange(nullptr);
        VirtualFree(wrong, 0, MEM_RELEASE);
        g_testTarget = reinterpret_cast<uintptr_t>(code);
    }
    const SkinHookState armed = skinEntityHookArm(text, sizeof(text));
    if (armed != SkinHookState::Armed || !std::strstr(text, "armed") || !std::strstr(text, "READ ONLY")) fail("H3.a", "the hook did not arm on the synthetic function");
    if (skinEntityHookArm(text, sizeof(text)) != SkinHookState::Armed) fail("H3.b", "arming twice changed the state");
    // gate on: a call is observed, after the original ran (the end row is zero until the original writes it, so reading it first would show)
    put32(node + 0xC4, 0);
    call(node, 0, 0, 0);
    if (!skinEntityHookLatest(snap)) fail("H4.a", "no snapshot after a call");
    else {
        if (snap.n != 3 || snap.e[0].dst != 1 || snap.e[1].dst != 41 || snap.e[2].dst != 91 || snap.e[0].count != 40 || snap.e[2].count != 30) fail("H4.b", "the entries were read wrongly");
        if (snap.end != 0x4D) fail("H4.c", "the end row was read before the original ran (original FIRST)");
        if (snap.e[0].key != entries[0] || snap.e[1].vtable == snap.e[0].vtable) fail("H4.d", "keys or vtables were wrong");
        if (snap.tid != GetCurrentThreadId() || snap.node != node || snap.seq != 1 || snap.flags != 0) fail("H4.e", "snapshot metadata wrong");
    }
    char event[400];
    if (!skinEntityHookNextEvent(event, sizeof(event)) || !std::strstr(event, "first call") || !std::strstr(event, "thread")) fail("H5.a", "no first-call event");
    // the gate shut: the original still runs, nothing is observed
    skinEntityHookSetGate(false);
    put32(node + 0xC4, 0);
    call(node, 0, 0, 0);
    uint32_t endAfter = 0;
    std::memcpy(&endAfter, reinterpret_cast<void*>(node + 0xC4), 4);
    if (endAfter != 0x4D) fail("H6.a", "the original did not run with the gate shut");
    if (skinEntityHookStats().calls != 1) fail("H6.b", "a call was observed with the gate shut");
    skinEntityHookSetGate(true);
    // a fault: a next pointer into memory that is not mapped
    {
        auto* hole = static_cast<uint8_t*>(VirtualAlloc(nullptr, 65536, MEM_RESERVE, PAGE_NOACCESS));
        const uint64_t unmapped = reinterpret_cast<uint64_t>(hole);
        put64(entries[1] + 8, unmapped);
        call(node, 0, 0, 0);
        if (!skinEntityHookLatest(snap) || !(snap.flags & skinjoin::kSnapFault) || snap.n != 2) fail("H7.a", "an unmapped entry was not caught as a fault");
        put64(entries[1] + 8, entries[2]);
        VirtualFree(hole, 0, MEM_RELEASE);
    }
    // an implausible pointer
    put64(entries[2] + 8, 0x1234);
    call(node, 0, 0, 0);
    if (!skinEntityHookLatest(snap) || !(snap.flags & skinjoin::kSnapImplausible) || snap.n != 3) fail("H8.a", "a wild next pointer was not flagged implausible");
    put64(entries[2] + 8, 0);
    // a list that points back into itself ends at the cap
    put64(entries[2] + 8, entries[0]);
    call(node, 0, 0, 0);
    if (!skinEntityHookLatest(snap) || !(snap.flags & skinjoin::kSnapOverflow) || snap.n != skinjoin::kMaxEntries) fail("H9.a", "a cycle did not end at the snapshot's capacity");
    put64(entries[2] + 8, 0);
    // good again, and the sequence numbers run on (observed calls so far: the first read, the fault, the wild pointer, the cycle, this one)
    call(node, 0, 0, 0);
    if (!skinEntityHookLatest(snap) || snap.flags != 0 || snap.seq != 5) fail("H10.a", "the sequence or the recovery was wrong");
    // a different thread is noticed
    {
        struct Arg { uint64_t node; void (__fastcall* fn)(uintptr_t, uintptr_t, uintptr_t, uintptr_t); } arg{node, call};
        HANDLE t = CreateThread(nullptr, 0, [](void* p) -> DWORD { auto* a = static_cast<Arg*>(p); a->fn(uintptr_t(a->node), 0, 0, 0); return 0; }, &arg, 0, nullptr);
        WaitForSingleObject(t, 5000);
        CloseHandle(t);
        bool other = false;
        while (skinEntityHookNextEvent(event, sizeof(event))) other = other || std::strstr(event, "another thread") != nullptr;
        if (!other || skinEntityHookStats().threads != 2) fail("H11.a", "a call from another thread was not reported");
    }
    // a reader whose copy is overtaken: the writer laps the slot between the reader's copy and its check, and the reader must not hand out the copy
    {
        s_lapNode = node;
        s_lapCall = call;
        g_testMidRead = &lapTheWriter;
        const bool got = skinEntityHookLatest(snap);
        const uint64_t calls = skinEntityHookStats().calls;
        bool whole = got && snap.n == 3 && snap.flags == 0;
        for (uint32_t k = 0; k < snap.n && whole; ++k) whole = snap.e[k].key == entries[k];
        if (!got || !whole || snap.seq != calls) fail("H12.c", "a copy overtaken by the writer was handed out (or no fresh one was found)");
    }
    // a reader against a writer: every copy is one call's list, never a mix
    {
        struct Arg { uint64_t node; void (__fastcall* fn)(uintptr_t, uintptr_t, uintptr_t, uintptr_t); std::atomic<bool> stop{false}; } arg{node, call};
        HANDLE t = CreateThread(nullptr, 0, [](void* p) -> DWORD { auto* a = static_cast<Arg*>(p); while (!a->stop.load()) { a->fn(uintptr_t(a->node), 0, 0, 0); for (int spin = 0; spin < 2000; ++spin) YieldProcessor(); } return 0; }, &arg, 0, nullptr);
        // Not a fixed number of tries: under load either thread may go unscheduled for a while, so the reader runs until it has seen enough
        // whole copies or ten seconds have passed. Every copy it does get must be one call's list. The writer pauses a few microseconds between
        // calls (the game calls once per frame; a writer with no pause starves any reader, which is a property of the test, not of the hook).
        unsigned good = 0, bad = 0;
        uint64_t last = 0;
        const ULONGLONG started = GetTickCount64();
        while (good < 100 && GetTickCount64() - started < 10000) {
            if (!skinEntityHookLatest(snap)) { SwitchToThread(); continue; }
            bool consistent = snap.n == 3 && snap.flags == 0 && snap.seq >= last && snap.node == node;
            for (uint32_t k = 0; k < snap.n && consistent; ++k) consistent = snap.e[k].key == entries[k] && snap.e[k].dst == (k == 0 ? 1u : k == 1 ? 41u : 91u);
            last = snap.seq;
            consistent ? ++good : ++bad;
        }
        arg.stop.store(true);
        WaitForSingleObject(t, 5000);
        CloseHandle(t);
        char what[160];
        std::snprintf(what, sizeof(what), "a snapshot was torn (%u whole, %u torn) while the game's thread kept calling", good, bad);
        if (bad) fail("H12.a", what);
        std::snprintf(what, sizeof(what), "only %u whole snapshots were read in ten seconds", good);
        if (good < 100) fail("H12.b", what);
    }
    // a second node stands the hook down
    {
        std::vector<uint64_t> other;
        const uint32_t c2[1] = {10};
        const uint64_t node2 = layout(heap, 1, c2, 1, other);
        call(node2, 0, 0, 0);
        if (skinEntityHookState() != SkinHookState::StoodDown) fail("H13.a", "a second node did not stand the hook down");
        bool said = false;
        while (skinEntityHookNextEvent(event, sizeof(event))) said = said || std::strstr(event, "stood down") != nullptr;
        if (!said) fail("H13.b", "no stand-down event");
        if (skinEntityHookLatest(snap)) fail("H13.c", "a snapshot is offered after the stand-down");
        call(node, 0, 0, 0);   // and the original still runs
        std::memcpy(&endAfter, reinterpret_cast<void*>(node + 0xC4), 4);
        if (endAfter != 0x4D) fail("H13.d", "the original stopped running after the stand-down");
    }
    // H14..H17: what stands the hook down, judged on a State of its own (the real observe() and noteChain(), no patch: a hook that stood down cannot be
    // armed again, so every case gets a fresh one). The F12 flight: 120 lists of no entries, made on the main menu, stood the hook down before a character existed.
    {
        const auto fresh = [] { State* t = new State; t->state.store(int(SkinHookState::Armed)); return t; };
        const auto armedState = [](State* t) { return t->state.load() == int(SkinHookState::Armed); };
        std::vector<uint64_t> scratch;
        const uint32_t noCount[1] = {0};
        const uint32_t oneBone[1] = {20};
        const uint64_t emptyNode = layout(heap, 0, noCount, 1, scratch);       // no entries, end row 1: the assembler had nobody to skin
        const uint64_t emptyOddNode = layout(heap, 0, noCount, 5, scratch);    // no entries but an end row of 5: not a clean empty list
        std::vector<uint64_t> badEntry, goodEntry;
        const uint64_t badNode = layout(heap, 1, noCount, 1, badEntry);        // one entry of no bones: something to read, and unusable
        layout(heap, 1, oneBone, 1, goodEntry);                                // an entry of 20 bones (ending at row 21): the good list when hung on a node
        // ONE node whose list the cases rewrite between calls (a second node would stand the hook down by itself, H13)
        const uint64_t mixNode = layout(heap, 0, noCount, 1, scratch);
        const auto setEmpty = [&] { put64(mixNode + 0xA8, 0); put32(mixNode + 0xC4, 1); };
        const auto setBad = [&] { put64(mixNode + 0xA8, badEntry[0]); put32(mixNode + 0xC4, 1); };
        const auto setGood = [&] { put64(mixNode + 0xA8, goodEntry[0]); put32(mixNode + 0xC4, 21); };
        // H14: lists of no entries judge nothing
        {
            State* t = fresh();
            for (int i = 0; i < 200; ++i) observe(*t, emptyNode);
            if (!armedState(t)) fail("H14.a", "200 lists of no entries stood the hook down (nothing had anyone to skin)");
            if (t->calls.load() != 200 || t->emptyLists.load() != 200 || t->judged.load() != 0 || t->usable.load() != 0)
                fail("H14.b", "the clean empty lists were not counted as such, or were counted as judged");
            for (int i = 0; i < 300; ++i) noteChain(*t, 0);
            if (!armedState(t) || t->emptyWithJobs.load() != 0) fail("H14.c", "dispatches with no jobs judged an empty list");
            delete t;
        }
        // H15: the bound stays for lists with something to read
        {
            State* t = fresh();
            for (int i = 0; i < 119; ++i) observe(*t, badNode);
            if (!armedState(t)) fail("H15.a", "the hook stood down before its 120th list with something to read");
            observe(*t, badNode);
            if (armedState(t) || !std::strstr(t->why, "none of the first 120")) fail("H15.b", "120 unusable lists with something to read did not stand the hook down");
            delete t;
            t = fresh();
            for (int i = 0; i < 119; ++i) observe(*t, emptyOddNode);
            observe(*t, emptyOddNode);
            if (armedState(t)) fail("H15.c", "120 lists of no entries but an end row of 5 (not a clean empty list) did not stand the hook down");
            delete t;
            // a list the walk could not read (its head points into unmapped memory) has no entries either, and is not a clean empty one
            {
                auto* hole = static_cast<uint8_t*>(VirtualAlloc(nullptr, 65536, MEM_RESERVE, PAGE_NOACCESS));
                t = fresh();
                put64(mixNode + 0xA8, reinterpret_cast<uint64_t>(hole));
                put32(mixNode + 0xC4, 1);
                for (int i = 0; i < 120; ++i) observe(*t, mixNode);
                if (armedState(t) || t->faulted.load() != 120) fail("H15.f", "120 lists whose head faulted (no entries, a walk flag) did not stand the hook down");
                delete t;
                VirtualFree(hole, 0, MEM_RELEASE);
            }
            // empty lists in between do not reset or advance the count
            t = fresh();
            for (int i = 0; i < 119; ++i) { setBad(); observe(*t, mixNode); setEmpty(); observe(*t, mixNode); }
            if (!armedState(t) || t->judged.load() != 119) fail("H15.d", "empty lists between unusable ones advanced the count");
            setBad();
            observe(*t, mixNode);
            if (armedState(t)) fail("H15.e", "the 120th unusable list with empty ones between did not stand the hook down");
            delete t;
        }
        // H16: an empty list is judged by a dispatch that has jobs
        {
            State* t = fresh();
            for (int i = 0; i < 119; ++i) { observe(*t, emptyNode); noteChain(*t, 50); }
            if (!armedState(t) || t->emptyWithJobs.load() != 119) fail("H16.a", "the hook stood down early, or the empty lists a dispatch with jobs met were not counted");
            observe(*t, emptyNode);
            noteChain(*t, 50);
            if (armedState(t) || !std::strstr(t->why, "had jobs on 120")) fail("H16.b", "120 dispatches with jobs that met an empty list did not stand the hook down");
            delete t;
        }
        // H17: one usable list ends the judging for good
        {
            State* t = fresh();
            setGood();
            observe(*t, mixNode);
            if (t->usable.load() != 1) fail("H17.a", "the synthetic good list was not usable");
            for (int i = 0; i < 300; ++i) { setEmpty(); observe(*t, mixNode); noteChain(*t, 50); setBad(); observe(*t, mixNode); }
            if (!armedState(t)) fail("H17.b", "the hook stood down after it had read a usable list");
            delete t;
        }
    }
    return failures;
}
#endif

}  // namespace edvr
