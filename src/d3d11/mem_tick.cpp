#include "mem_tick.h"

#include <windows.h>
#include <d3d11.h>
#include <psapi.h>

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

#include "../common/guard.h"
#include "../common/log.h"
#include "../common/mem_watch.h"
#include "../common/timing.h"
#include "../common/vram_query.h"

// The glue for the process memory watch (src\common\mem_watch.h says what is written and when; docs/memory-growth-2026-10-22.md
// says why). One thread of its own samples, so the render thread pays one atomic read a frame and nothing else.
//
// Three kinds of source, from the safest to the least: the OS (the process's counters, the machine's commit, DXGI's video
// memory, the process list), Elite's own GPU resource accounting, and Elite's CPU heap manager. The last two are reads of the
// game's memory at offsets taken from one build, so they are read only after the build is proved by its PE identity and by
// the bytes of the instructions that use those offsets, and every read is under SEH.

namespace edvr {
namespace {

// ---- what the thread keeps -------------------------------------------------------------------------------------------

std::atomic<bool> g_started{false};

// The adapter's IDXGIAdapter3: one reference, taken at the first Present and never released (vram_tick.cpp holds its own the
// same way). DXGI's adapter objects are free-threaded, so the sampling thread may call it.
IDXGIAdapter3* g_adapter = nullptr;
const char* g_adapterWhy = "no adapter was asked for";

FaultBudget g_budget("memWatch", 3);

// The sampling thread's alone.
MemPolicy g_policy;

// The process list: one buffer for the process, grown when the list outgrows it (SystemProcessInformation is the only class
// that needs it) and never given back. A few hundred KB on a machine with a few hundred processes.
uint8_t* g_scan = nullptr;
ULONG g_scanBytes = 0;
constexpr ULONG kScanFirstBytes = 1u << 20;
constexpr ULONG kScanMostBytes = 64u << 20;

// ---- the reasons a group is missing, kept for the armed line ------------------------------------------------------------

struct Notes {
    MemUnavailable missing;
    char text[kMemGroups][160] = {};
    void fail(MemGroup g, const char* fmt, ...) {
        const int i = static_cast<int>(g);
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(text[i], sizeof(text[i]), fmt, ap);
        va_end(ap);
        missing.why[i] = text[i];
    }
};

// ---- SEH reads of the game's memory -----------------------------------------------------------------------------------
//
// Each is its own small function with plain data in it (no object with a destructor in a function that has a __try), the
// discipline vertex_resync_hook.cpp and flat_camera_inject.cpp keep. noinline so the handler is a real frame.

__declspec(noinline) bool sehRead(uintptr_t address, void* out, size_t bytes) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) bool sehMatches(uintptr_t address, const uint8_t* expected, size_t bytes) noexcept {
    __try {
        return std::memcmp(reinterpret_cast<const void*>(address), expected, bytes) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// An address in the range user mode can have. Not a proof it is mapped (the read is what proves that); a cheap refusal of
// the values a half-written structure holds.
inline bool userPointer(uint64_t p) { return p >= 0x10000ull && p < 0x00007FFFFFFE0000ull; }

// ---- Elite build 334473: the identity, the pins, the offsets -------------------------------------------------------------
//
// EliteDangerous64.exe, SHA-256 6891f70a...; image base 0x140000000, every address below an RVA from it. The same pair the
// other build-bound hooks check (vertex_resync_core.h), then the bytes of the instructions that carry each displacement used
// below: if the exe at these RVAs is not the one the offsets were read from, nothing is read.

constexpr uint32_t kEliteStamp = 1791840573u;
constexpr uint32_t kEliteImageSize = 104906752u;

struct Pin {
    uint32_t rva;
    uint8_t length;
    uint8_t bytes[36];
};

// GPU resource accounting. The counter update (0x51FA70: add to [device + kind*8 + 0x1EE0], the sum's high water stored at
// +0x1F40, the second set added at +0x1F48), the load of the renderer global and the device at +0x18 of it (0x51D740, the
// "No device created as yet" report), and the heap loads of the game's own report (0x519160).
const Pin kGpuPins[] = {
    {0x51FB32, 8, {0x4C, 0x01, 0xB4, 0x2A, 0xE0, 0x1E, 0x00, 0x00}},         // add [rdx+rbp+0x1EE0], r14
    {0x51FB9C, 7, {0x48, 0x89, 0x8D, 0x40, 0x1F, 0x00, 0x00}},               // mov [rbp+0x1F40], rcx
    {0x51FBA9, 8, {0x4C, 0x01, 0xB4, 0x2A, 0x48, 0x1F, 0x00, 0x00}},         // add [rdx+rbp+0x1F48], r14
    {0x51D746, 7, {0x48, 0x8B, 0x05, 0x5B, 0x05, 0xA0, 0x05}},               // mov rax, [rip+...] = [0x5F1DCA8]
    {0x51D755, 4, {0x48, 0x8B, 0x48, 0x18}},                                 // mov rcx, [rax+0x18]
    {0x519D52, 7, {0x48, 0x8B, 0x86, 0x00, 0x20, 0x00, 0x00}},               // mov rax, [rsi+0x2000]
    {0x519D5C, 11, {0x4C, 0x8B, 0x3C, 0xC8, 0x49, 0x8B, 0x8F, 0xB0, 0x00, 0x00, 0x00}},   // heap = rax[rcx*8]; mov rcx, [heap+0xB0]
    {0x519D88, 7, {0x49, 0x8B, 0x8F, 0xC8, 0x04, 0x00, 0x00}},               // mov rcx, [heap+0x4C8]
    {0x519E0B, 7, {0x41, 0x8B, 0x87, 0x10, 0x05, 0x00, 0x00}},               // mov eax, [heap+0x510]
    {0x51A0EB, 7, {0x48, 0x3B, 0x8E, 0xF8, 0x1F, 0x00, 0x00}},               // cmp rcx, [rsi+0x1FF8]
};

// CPU heap manager. The heap constructor's registration in the table (0x537320: the global, the count at +0x1510, the entry
// at +0x1400 + 16*count as {heap, heap+0x10}), the default heap at +0x1500 (0x543F90), the byte count added at +0x50 of the
// allocator that sits at heap+0x10 (0x553480), the statistics block loaded from +0x108 of the allocator state at heap+0xA0
// and updated (count +0x20, current +0x10, peak +0x18) in 0x540370, and the two offsets that relate heap, allocator and state.
const Pin kCpuPins[] = {
    {0x537385, 7, {0x48, 0x8B, 0x1D, 0xCC, 0x6A, 0x9E, 0x05}},               // mov rbx, [rip+...] = [0x5F1DE58]
    {0x5373A0, 36, {0x48, 0x8B, 0x83, 0x10, 0x15, 0x00, 0x00, 0x48, 0x8D, 0x90, 0x40, 0x01, 0x00, 0x00, 0x48, 0x03, 0xD2, 0x48, 0xFF, 0xC0,
                    0x48, 0x89, 0x83, 0x10, 0x15, 0x00, 0x00, 0x48, 0x89, 0x3C, 0xD3, 0x4C, 0x89, 0x74, 0xD3, 0x08}},   // the table entry
    {0x543FED, 14, {0x48, 0x8B, 0x05, 0x64, 0x9E, 0x9D, 0x05, 0x48, 0x8B, 0x88, 0x00, 0x15, 0x00, 0x00}},               // [manager+0x1500]
    {0x5536B3, 4, {0x48, 0x01, 0x5F, 0x50}},                                 // add [rdi+0x50], rbx
    {0x5404AE, 10, {0x48, 0x8B, 0x87, 0x08, 0x01, 0x00, 0x00, 0xFF, 0x40, 0x20}},   // mov rax, [rdi+0x108]; inc dword [rax+0x20]
    {0x5404D1, 21, {0x48, 0x01, 0x42, 0x10, 0x48, 0x8B, 0x4A, 0x10, 0x48, 0x39, 0x4A, 0x18, 0x48, 0x0F, 0x43, 0x4A, 0x18, 0x48, 0x89, 0x4A, 0x18}},
    {0x54C9DF, 4, {0x48, 0x8D, 0x4B, 0x10}},                                 // lea rcx, [rbx+0x10]: heap -> allocator
    {0x55B695, 7, {0x48, 0x8D, 0x8F, 0x90, 0x00, 0x00, 0x00}},               // lea rcx, [rdi+0x90]: allocator -> state
};

// Offsets (RVAs and displacements) the pins stand for.
constexpr uintptr_t kRendererGlobalRva = 0x5F1DCA8;   // qword: the renderer
constexpr uintptr_t kRendererDevice = 0x18;           // renderer -> device D, null before the device exists
constexpr uintptr_t kGpuKinds = 0x1EE0;               // D: twelve u64 live byte counters
constexpr uintptr_t kGpuHighWater = 0x1F40;           // D: u64, the most their sum has been
constexpr uintptr_t kGpuSecond = 0x1F48;              // D: twelve u64, the same without flagged resources
constexpr uintptr_t kGpuHeapCount = 0x1FF8;           // D: u64
constexpr uintptr_t kGpuHeapArray = 0x2000;           // D: pointer to the heap pointers
constexpr uintptr_t kHeapUsed = 0xB0;                 // heap: u64, bytes (descriptors for a descriptor heap)
constexpr uintptr_t kHeapCapacity = 0x4C8;            // heap: u64, same unit
constexpr uintptr_t kHeapType = 0x510;                // heap: int32, 3 is a descriptor heap (the game's report prints it so)
constexpr int32_t kHeapTypeDescriptors = 3;

constexpr uintptr_t kManagerGlobalRva = 0x5F1DE58;    // qword: the heap manager, the static object below
constexpr uintptr_t kManagerObjectRva = 0x5F28120;
constexpr uintptr_t kManagerTable = 0x1400;           // 16 entries of {heap, heap + 0x10}
constexpr uintptr_t kManagerDefaultHeap = 0x1500;     // qword
constexpr uintptr_t kManagerCount = 0x1510;           // u64
constexpr uintptr_t kHeapBytesFromOs = 0x60;          // heap: u64, bytes its allocator took from the C runtime heap (0x553480 adds to it)
constexpr uintptr_t kHeapStats = 0x1A8;               // heap: pointer to the allocator's statistics block
constexpr uintptr_t kStatsParent = 0x08;              // u64, 0 for the block that accumulates
constexpr uintptr_t kStatsCurrent = 0x10;             // u64, bytes handed out now
constexpr uintptr_t kStatsPeak = 0x18;                // u64
constexpr uintptr_t kStatsAllocs = 0x20;              // u32, allocations made (the free path does not take it down)

struct EliteGate {
    bool probed = false;
    uintptr_t base = 0;
    bool egpu = false;
    bool eheap = false;
    char egpuWhy[160] = {};
    char eheapWhy[160] = {};
};
EliteGate g_elite;

bool pinsHold(uintptr_t base, const Pin* pins, size_t count, uint32_t* failedRva) {
    for (size_t i = 0; i < count; ++i) {
        if (!sehMatches(base + pins[i].rva, pins[i].bytes, pins[i].length)) {
            *failedRva = pins[i].rva;
            return false;
        }
    }
    return true;
}

// Once, at the armed sample: is this process Elite build 334473, and do the instructions behind each group's offsets read as
// they did there.
void probeElite(EliteGate* gate) {
    gate->probed = true;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    gate->base = base;
    const char* why = nullptr;
    if (!base) {
        why = "this process has no module image";
    } else {
        uint32_t peOffset = 0, stamp = 0, size = 0;
        if (!sehRead(base + 0x3C, &peOffset, 4) || peOffset > 0x1000 || !sehRead(base + peOffset + 8, &stamp, 4) ||
            !sehRead(base + peOffset + 0x50, &size, 4)) {
            why = "the PE header could not be read";
        } else if (stamp != kEliteStamp || size != kEliteImageSize) {
            why = "this is not Elite build 334473 (PE timestamp or image size differs)";
        }
    }
    if (why) {
        std::snprintf(gate->egpuWhy, sizeof(gate->egpuWhy), "%s", why);
        std::snprintf(gate->eheapWhy, sizeof(gate->eheapWhy), "%s", why);
        return;
    }
    uint32_t failed = 0;
    if (pinsHold(base, kGpuPins, sizeof(kGpuPins) / sizeof(kGpuPins[0]), &failed)) gate->egpu = true;
    else std::snprintf(gate->egpuWhy, sizeof(gate->egpuWhy), "the instructions at +0x%X are not those of build 334473", static_cast<unsigned>(failed));
    if (pinsHold(base, kCpuPins, sizeof(kCpuPins) / sizeof(kCpuPins[0]), &failed)) gate->eheap = true;
    else std::snprintf(gate->eheapWhy, sizeof(gate->eheapWhy), "the instructions at +0x%X are not those of build 334473", static_cast<unsigned>(failed));
}

// ---- Elite's GPU resource accounting -----------------------------------------------------------------------------------

enum class Fetch { Ok, Unread, Bad };

struct EgpuRaw {
    uint64_t kind[kMemGpuKinds];
    uint64_t second[kMemGpuKinds];
    uint64_t highWater;
    uint64_t heapCount;
    bool heapsRead;   // every heap in the array was read
    uint64_t heapUsed[kMemGpuHeapMax];
    uint64_t heapCapacity[kMemGpuHeapMax];
    int32_t heapType[kMemGpuHeapMax];
};

Fetch readEgpu(uintptr_t base, EgpuRaw* r) {
    std::memset(r, 0, sizeof(*r));
    uint64_t renderer = 0, device = 0;
    if (!sehRead(base + kRendererGlobalRva, &renderer, sizeof(renderer))) return Fetch::Bad;
    if (renderer == 0) return Fetch::Unread;
    if (!userPointer(renderer) || !sehRead(renderer + kRendererDevice, &device, sizeof(device))) return Fetch::Bad;
    if (device == 0) return Fetch::Unread;
    uint64_t heaps = 0;
    if (!userPointer(device) || !sehRead(device + kGpuKinds, r->kind, sizeof(r->kind)) ||
        !sehRead(device + kGpuHighWater, &r->highWater, sizeof(r->highWater)) ||
        !sehRead(device + kGpuSecond, r->second, sizeof(r->second)) ||
        !sehRead(device + kGpuHeapCount, &r->heapCount, sizeof(r->heapCount)) ||
        !sehRead(device + kGpuHeapArray, &heaps, sizeof(heaps)))
        return Fetch::Bad;
    r->heapsRead = true;
    if (r->heapCount > kMemGpuHeapMax) return Fetch::Ok;   // the check below refuses it; no heap is followed
    for (uint64_t i = 0; i < r->heapCount && r->heapsRead; ++i) {
        uint64_t heap = 0;
        if (!userPointer(heaps) || !sehRead(heaps + i * 8, &heap, sizeof(heap)) || !userPointer(heap) ||
            !sehRead(heap + kHeapUsed, &r->heapUsed[i], sizeof(uint64_t)) ||
            !sehRead(heap + kHeapCapacity, &r->heapCapacity[i], sizeof(uint64_t)) ||
            !sehRead(heap + kHeapType, &r->heapType[i], sizeof(int32_t)))
            r->heapsRead = false;
    }
    return Fetch::Ok;
}

bool egpuPlausible(const EgpuRaw& r) {
    if (!memGpuCountersPlausible(r.kind, r.second, r.highWater)) return false;
    if (!memGpuHeapCountPlausible(r.heapCount) || !r.heapsRead) return false;
    for (uint64_t i = 0; i < r.heapCount; ++i) {
        if (!memGpuHeapPlausible(r.heapUsed[i], r.heapCapacity[i])) return false;
    }
    return true;
}

// The counters are updated by the game's threads under its own lock, which this thread does not take: between the add to a
// counter and the store of the high water, the sum is briefly above it. A read that fails its checks is read again, twice,
// before the sample is called implausible.
MemEgpu sampleEgpu(uintptr_t base) {
    MemEgpu out;
    out.state = MemRead::Implausible;
    EgpuRaw raw;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const Fetch fetched = readEgpu(base, &raw);
        if (fetched == Fetch::Unread) {
            out.state = MemRead::Unread;
            return out;
        }
        if (fetched == Fetch::Ok && egpuPlausible(raw)) {
            out.state = MemRead::Ok;
            for (uint32_t i = 0; i < kMemGpuKinds; ++i) out.kind[i] = raw.kind[i];
            out.highWater = raw.highWater;
            out.heaps = raw.heapCount;
            for (uint64_t i = 0; i < raw.heapCount; ++i) {
                if (raw.heapType[i] == kHeapTypeDescriptors) {
                    ++out.descriptorHeaps;   // counted in descriptors: not added to the byte sums
                } else {
                    out.heapUsed += raw.heapUsed[i];
                    out.heapCapacity += raw.heapCapacity[i];
                }
            }
            return out;
        }
        Sleep(0);
    }
    return out;
}

// ---- Elite's CPU heap manager --------------------------------------------------------------------------------------------

struct EheapRaw {
    uint64_t manager;
    uint64_t count;
    bool tableRead;   // every entry up to the count was a heap whose allocator pointer is heap + 0x10
    uint64_t osBytes[kMemCpuHeapMax];
    bool mainRead;
    bool mainAccounted;
    uint64_t mainCurrent;
    uint64_t mainPeak;
    uint32_t mainAllocs;
};

Fetch readEheap(uintptr_t base, EheapRaw* r) {
    std::memset(r, 0, sizeof(*r));
    if (!sehRead(base + kManagerGlobalRva, &r->manager, sizeof(r->manager))) return Fetch::Bad;
    if (r->manager == 0) return Fetch::Unread;
    if (r->manager != base + kManagerObjectRva) return Fetch::Bad;   // the global points at the static object, or this is not the layout
    const uintptr_t manager = static_cast<uintptr_t>(r->manager);
    if (!sehRead(manager + kManagerCount, &r->count, sizeof(r->count))) return Fetch::Bad;
    r->tableRead = true;
    if (r->count > kMemCpuHeapMax) return Fetch::Ok;   // refused by the check; no entry is followed
    for (uint64_t i = 0; i < r->count && r->tableRead; ++i) {
        uint64_t entry[2] = {};
        if (!sehRead(manager + kManagerTable + i * 16, entry, sizeof(entry)) || !userPointer(entry[0]) || entry[1] != entry[0] + 0x10 ||
            !sehRead(static_cast<uintptr_t>(entry[0]) + kHeapBytesFromOs, &r->osBytes[i], sizeof(uint64_t)))
            r->tableRead = false;
    }
    // The default heap is the one the game allocates from when a thread has set no other; its allocator's statistics block
    // is the one that follows the bytes handed out.
    uint64_t heap = 0, stats = 0, parent = 0;
    if (sehRead(manager + kManagerDefaultHeap, &heap, sizeof(heap)) && userPointer(heap) &&
        sehRead(static_cast<uintptr_t>(heap) + kHeapStats, &stats, sizeof(stats)) && userPointer(stats) &&
        sehRead(static_cast<uintptr_t>(stats) + kStatsParent, &parent, sizeof(parent)) &&
        sehRead(static_cast<uintptr_t>(stats) + kStatsCurrent, &r->mainCurrent, sizeof(uint64_t)) &&
        sehRead(static_cast<uintptr_t>(stats) + kStatsPeak, &r->mainPeak, sizeof(uint64_t)) &&
        sehRead(static_cast<uintptr_t>(stats) + kStatsAllocs, &r->mainAllocs, sizeof(uint32_t))) {
        r->mainRead = true;
        r->mainAccounted = parent == 0;
    }
    return Fetch::Ok;
}

bool eheapPlausible(const EheapRaw& r, uint64_t osSum) {
    if (!memCpuHeapCountPlausible(r.count) || !r.tableRead) return false;
    if (!memCpuOsBytesPlausible(osSum)) return false;
    return !r.mainRead || !r.mainAccounted || memCpuMainPlausible(r.mainCurrent, r.mainPeak);
}

MemEheap sampleEheap(uintptr_t base) {
    MemEheap out;
    out.state = MemRead::Implausible;
    EheapRaw raw;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const Fetch fetched = readEheap(base, &raw);
        if (fetched == Fetch::Unread) {
            out.state = MemRead::Unread;
            return out;
        }
        if (fetched == Fetch::Ok) {
            uint64_t osSum = 0;
            bool eachOk = raw.count <= kMemCpuHeapMax;
            for (uint64_t i = 0; eachOk && i < raw.count; ++i) {
                if (!memCpuOsBytesPlausible(raw.osBytes[i])) eachOk = false;   // so the sum cannot overflow
                else osSum += raw.osBytes[i];
            }
            if (eachOk && eheapPlausible(raw, osSum)) {
                out.state = MemRead::Ok;
                out.heaps = raw.count;
                out.osBytes = osSum;
                out.mainRead = raw.mainRead;
                out.mainAccounted = raw.mainAccounted;
                out.mainCurrent = raw.mainCurrent;
                out.mainPeak = raw.mainPeak;
                out.mainAllocs = raw.mainAllocs;
                return out;
            }
        }
        Sleep(0);
    }
    return out;
}

// ---- the OS -------------------------------------------------------------------------------------------------------------

MemMachine readMachine(DWORD* error) {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) {
        *error = GetLastError();
        return MemMachine{};
    }
    return memMachineOf(ms.ullTotalPageFile, ms.ullAvailPageFile, ms.ullAvailPhys);
}

void sampleProcess(MemFigures* f, Notes* notes) {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        f->process.valid = true;
        // PagefileUsage, not PrivateUsage: on Windows both are the process's commit charge, but Wine (Proton) fills only
        // the plain PROCESS_MEMORY_COUNTERS and leaves PrivateUsage at 0, while its PagefileUsage carries the process's
        // anonymous resident memory plus swap.
        f->process.commit = pmc.PagefileUsage;
        f->process.commitPeak = pmc.PeakPagefileUsage;
        f->process.workingSet = pmc.WorkingSetSize;
    } else {
        notes->fail(MemGroup::Process, "GetProcessMemoryInfo failed, error %lu", GetLastError());
    }
    // A Windows process always holds handles; 0 is Wine not counting them, and is left out rather than logged as a count.
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles) && handles != 0) {
        f->handlesValid = true;
        f->handles = handles;
    }
}

void sampleVram(MemFigures* f, Notes* notes) {
    if (!g_adapter) {
        notes->fail(MemGroup::Vram, "%s", g_adapterWhy);
        return;
    }
    HRESULT hr = S_OK;
    const VramSegment local = vramReadSegment(g_adapter, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &hr);
    if (!local.valid) {
        notes->fail(MemGroup::Vram, "QueryVideoMemoryInfo failed for the local segment group, hr 0x%08lX", static_cast<unsigned long>(hr));
        return;
    }
    f->local.valid = true;
    f->local.used = local.usage;
    f->local.budget = local.budget;
}

// ntdll's SYSTEM_PROCESS_INFORMATION as far as it is used, x64. The SDK's winternl.h hides most of it in reserved fields and
// declares VirtualSize as 32 bits, which moves every later field.
struct SystemProcessEntry {
    ULONG NextEntryOffset;                 // 0x00
    ULONG NumberOfThreads;                 // 0x04
    uint8_t Reserved1[0x30];               // 0x08: working set private size, hard faults, cycle time, create, user, kernel times
    USHORT ImageNameLength;                // 0x38, bytes
    USHORT ImageNameMaximumLength;         // 0x3A
    const wchar_t* ImageNameBuffer;        // 0x40, into the same buffer
    LONG BasePriority;                     // 0x48
    ULONG_PTR UniqueProcessId;             // 0x50
    ULONG_PTR InheritedFromUniqueProcessId;// 0x58
    ULONG HandleCount;                     // 0x60
    ULONG SessionId;                       // 0x64
    ULONG_PTR UniqueProcessKey;            // 0x68
    SIZE_T PeakVirtualSize;                // 0x70
    SIZE_T VirtualSize;                    // 0x78
    ULONG PageFaultCount;                  // 0x80
    SIZE_T PeakWorkingSetSize;             // 0x88
    SIZE_T WorkingSetSize;                 // 0x90
    SIZE_T QuotaPeakPagedPoolUsage;        // 0x98
    SIZE_T QuotaPagedPoolUsage;            // 0xA0
    SIZE_T QuotaPeakNonPagedPoolUsage;     // 0xA8
    SIZE_T QuotaNonPagedPoolUsage;         // 0xB0
    SIZE_T PagefileUsage;                  // 0xB8
    SIZE_T PeakPagefileUsage;              // 0xC0
    SIZE_T PrivatePageCount;               // 0xC8
};
static_assert(offsetof(SystemProcessEntry, ImageNameLength) == 0x38 && offsetof(SystemProcessEntry, ImageNameBuffer) == 0x40 &&
                  offsetof(SystemProcessEntry, UniqueProcessId) == 0x50 && offsetof(SystemProcessEntry, HandleCount) == 0x60 &&
                  offsetof(SystemProcessEntry, PrivatePageCount) == 0xC8 && sizeof(SystemProcessEntry) == 0xD0,
              "SYSTEM_PROCESS_INFORMATION layout");

using NtQuerySystemInformationFn = LONG(NTAPI*)(ULONG informationClass, PVOID buffer, ULONG length, PULONG returned);
constexpr ULONG kSystemProcessInformation = 5;
constexpr LONG kStatusInfoLengthMismatch = static_cast<LONG>(0xC0000004);

// This process's thread count, and the four VR runtimes by name. No other process's name is kept or written.
void sampleProcessList(MemFigures* f, Notes* notes) {
    static NtQuerySystemInformationFn query = nullptr;
    if (!query) {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        query = ntdll ? reinterpret_cast<NtQuerySystemInformationFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQuerySystemInformation")))
                      : nullptr;
        if (!query) {
            notes->fail(MemGroup::Scan, "ntdll does not export NtQuerySystemInformation");
            return;
        }
    }
    ULONG returned = 0;
    LONG status = kStatusInfoLengthMismatch;
    for (int attempt = 0; attempt < 6 && status == kStatusInfoLengthMismatch; ++attempt) {
        if (!g_scan || g_scanBytes == 0) {
            g_scan = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), 0, kScanFirstBytes));
            if (!g_scan) {
                notes->fail(MemGroup::Scan, "no memory for the process list");
                return;
            }
            g_scanBytes = kScanFirstBytes;
        }
        returned = 0;
        status = query(kSystemProcessInformation, g_scan, g_scanBytes, &returned);
        if (status == kStatusInfoLengthMismatch) {
            // The list grows between two calls: ask for what was said and some more.
            const ULONG want = returned + (256u << 10);
            if (want <= g_scanBytes || want > kScanMostBytes) {
                notes->fail(MemGroup::Scan, "the process list needs %lu bytes, more than this will hold", static_cast<unsigned long>(returned));
                return;
            }
            uint8_t* grown = static_cast<uint8_t*>(HeapReAlloc(GetProcessHeap(), 0, g_scan, want));
            if (!grown) {
                notes->fail(MemGroup::Scan, "no memory to grow the process list to %lu bytes", static_cast<unsigned long>(want));
                return;
            }
            g_scan = grown;
            g_scanBytes = want;
        }
    }
    if (status < 0) {
        notes->fail(MemGroup::Scan, "NtQuerySystemInformation failed, status 0x%08lX", static_cast<unsigned long>(status));
        return;
    }
    const uint8_t* const begin = g_scan;
    const uint8_t* const end = g_scan + (returned < g_scanBytes ? returned : g_scanBytes);
    const ULONG_PTR self = GetCurrentProcessId();
    const uint8_t* at = begin;
    while (static_cast<size_t>(end - at) >= sizeof(SystemProcessEntry)) {
        const SystemProcessEntry* e = reinterpret_cast<const SystemProcessEntry*>(at);
        if (e->UniqueProcessId == self) {
            f->threadsValid = true;
            f->threads = e->NumberOfThreads;
        }
        const wchar_t* name = e->ImageNameBuffer;
        if (name && e->ImageNameLength > 0 && (e->ImageNameLength & 1) == 0 &&
            reinterpret_cast<const uint8_t*>(name) >= begin &&
            static_cast<size_t>(end - reinterpret_cast<const uint8_t*>(name)) >= e->ImageNameLength) {
            const int which = memRuntimeIndexOf(name, e->ImageNameLength / 2u);
            if (which >= 0) {
                MemRuntime& r = f->runtime[which];
                r.running = true;
                r.commit += e->PrivatePageCount;
                r.handles += e->HandleCount;
            }
        }
        if (e->NextEntryOffset == 0 || e->NextEntryOffset < sizeof(SystemProcessEntry)) break;
        if (e->NextEntryOffset > static_cast<size_t>(end - at)) break;
        at += e->NextEntryOffset;
    }
}

// ---- the sample, and the thread ----------------------------------------------------------------------------------------

void sampleAll(const MemMachine& machine, DWORD machineError, MemFigures* f, Notes* notes) {
    f->machine = machine;
    if (!machine.valid) notes->fail(MemGroup::Machine, "GlobalMemoryStatusEx failed, error %lu", machineError);
    sampleProcess(f, notes);
    sampleVram(f, notes);
    sampleProcessList(f, notes);
    if (g_elite.egpu) f->egpu = sampleEgpu(g_elite.base);
    else notes->fail(MemGroup::Egpu, "%s", g_elite.egpuWhy);
    if (g_elite.eheap) f->eheap = sampleEheap(g_elite.base);
    else notes->fail(MemGroup::Eheap, "%s", g_elite.eheapWhy);
}

// One wake of the thread: the machine's commit against the policy, and the whole sample only when a line is due.
void wake() {
    DWORD machineError = 0;
    const MemMachine machine = readMachine(&machineError);
    const MemReason reason = g_policy.observe(nowMs(), machine);
    if (reason == MemReason::None) return;
    if (reason == MemReason::Armed) probeElite(&g_elite);
    MemFigures figures;
    Notes notes;
    sampleAll(machine, machineError, &figures, &notes);
    char line[kMemLineCap];
    if (reason == MemReason::Armed) formatMemArmed(line, sizeof(line), figures, notes.missing);
    else formatMemLine(line, sizeof(line), reason, figures);
    Log::get().noteReserved("%s", line);
}

void samplerMain() {
    SetThreadDescription(GetCurrentThread(), L"edvr-mem");
    for (;;) {
        if (!g_budget.shouldRun()) {
            guarded("memWatch/stop", [] {
                Log::get().noteReserved("mem: stopped -- the sampler faulted %d times; no further mem: line will be written this session.", 3);
            });
            return;
        }
        guardedBudget(g_budget, [] { wake(); });
        Sleep(static_cast<DWORD>(kMemWakeEveryMs));
    }
}

// The thread is made once, on the render thread's first Present, and lives for the process: it is never joined or destroyed
// (the rule stall_watch.cpp states for its watchdog, and the logger's), because it may be inside a read of the OS at any
// moment and the game never unloads this DLL.
void start(ID3D11Device* device) {
    g_adapter = vramAdapterOf(device, &g_adapterWhy);
    if (g_adapter) g_adapterWhy = "";
    bool started = false;
    try {
        std::thread(&samplerMain).detach();
        started = true;
    } catch (...) {
    }
    if (!started) {
        char line[kMemLineCap];
        formatMemUnavailable(line, sizeof(line), "the sampling thread could not be started");
        Log::get().noteReserved("%s", line);
    }
}

}  // namespace

void memWatchTick(ID3D11Device* device) {
    // The one read a frame costs: after the first call nothing below runs.
    if (g_started.load(std::memory_order_relaxed)) return;
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;
    guarded("memWatch/start", [&] { start(device); });
}

}  // namespace edvr
