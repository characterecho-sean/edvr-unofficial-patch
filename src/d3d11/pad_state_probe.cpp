// The pad-state probe (TEMPORARY, 2026-10-13). Four detours on the docking-state readers of build 332841 (0x12C3C20, 0x12CAF80, 0x12CADE0,
// 0x12CB320). Each reader takes its object (this) in rcx. The detour calls the ORIGINAL FIRST and keeps its return code, then makes plain reads
// of the object (this+0x3C8, [this-8] and its owner's +0x10, +0x2C, +0x2F) under __try. It calls no Elite function and logs nothing; the tick
// (the station's frame, on the render thread) prints. The relays are the store hook's: a gate word, closed means the original runs alone.
//
// Install: the PE TimeDateStamp and SizeOfImage first (the build's pair), then each reader's six-byte prologue pin. A refusal logs one line and is
// final. The pure rules and the line texts are in pad_state_core.h.
#include "pad_state_probe.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "../common/code_hook.h"
#include "../common/log.h"
#include "explorer_cam_core.h"
#include "pad_state_core.h"

namespace edvr {
namespace {

namespace pad = edvr::padstate;

constexpr size_t kRelayBytes = 44, kOriginalLiteral = 36;
using ReaderFn = uintptr_t(__fastcall*)(uintptr_t self);

struct Reader {
    std::atomic<uintptr_t> forward{0};
    CodeHook hook;
    uint8_t* relay = nullptr;
    std::atomic<bool> ready{false};
    std::atomic<bool> refused{false};
    const char* name = "";
    const char* status = "not installed yet";
};

// The observers' gate: the relays read this word first. Zero: the original runs alone (nothing is observed).
alignas(8) std::atomic<uintptr_t> g_padGate{0};
static_assert(decltype(g_padGate)::is_always_lock_free, "The relay reads the aligned atomic cell directly.");

Reader g_readers[pad::kReaders];

// The table, shared by the detours (the readers' thread) and the tick (the render thread): one short spinlock. Nothing logs under it.
std::atomic_flag g_padLock = ATOMIC_FLAG_INIT;
pad::PadTable g_table;
std::atomic<const char*> g_padEvent{nullptr};
bool g_attempted = false;        // the install ran (render thread)
uint64_t g_summaryStartMs = 0;   // the 30 s summary's window start (render thread)
uint64_t g_qpcFreq = 0;

struct SpinLock {
    SpinLock() noexcept {
        while (g_padLock.test_and_set(std::memory_order_acquire)) {
        }
    }
    ~SpinLock() { g_padLock.clear(std::memory_order_release); }
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;
};

uint64_t padQpcNow() noexcept {
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return static_cast<uint64_t>(q.QuadPart);
}

double qpcSeconds(uint64_t ticks) noexcept {
    if (g_qpcFreq == 0) return 0.0;
    return static_cast<double>(ticks) / static_cast<double>(g_qpcFreq);
}

// The plain reads of one object. POD only (a __try in this function, nothing to unwind). A fault leaves ok false.
bool readPadState(uintptr_t self, pad::PadReading* out) noexcept {
    __try {
        out->byte = *reinterpret_cast<const uint8_t*>(self + 0x3C8);
        const uintptr_t owner = *reinterpret_cast<const uintptr_t*>(self - 8);
        out->occ = *reinterpret_cast<const uint32_t*>(owner + 0x10);
        out->o2c = *reinterpret_cast<const uint8_t*>(owner + 0x2C);
        out->o2f = *reinterpret_cast<const uint8_t*>(owner + 0x2F);
        out->ok = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->ok = false;
        return false;
    }
}

// Reads the bytes at an address under __try (POD only). False when the read faults.
bool readBytes(uintptr_t address, uint8_t* out, size_t n) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The PE TimeDateStamp and SizeOfImage, read as the other build-keyed hooks read them (base+0x3C -> e_lfanew, +8, +0x50). POD only.
bool readIdentity(uintptr_t base, uint32_t* stamp, uint32_t* size) noexcept {
    __try {
        uint32_t peOff = 0;
        std::memcpy(&peOff, reinterpret_cast<const void*>(base + 0x3C), 4);
        if (peOff > 0x1000) return false;
        std::memcpy(stamp, reinterpret_cast<const void*>(base + peOff + 8), 4);
        std::memcpy(size, reinterpret_cast<const void*>(base + peOff + 0x50), 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Records one reader's call: the reading (plain reads, outside the lock), the QPC time, then the table under the spinlock. Never logs.
__declspec(noinline) void recordCall(size_t reader, uintptr_t self, uintptr_t result) noexcept {
    pad::PadReading r;
    readPadState(self, &r);
    const uint64_t qpc = padQpcNow();
    const int code = static_cast<int>(result & 0xFFu);
    SpinLock guard;
    pad::padNoteCall(g_table, reader, self, r, code, qpc);
}

// The four observers. Each runs the original first and keeps its return value (RAX), so the caller sees what it would have seen.
template <size_t N>
__declspec(noinline) uintptr_t __fastcall readerObserved(uintptr_t self) noexcept {
    const auto forward = reinterpret_cast<ReaderFn>(g_readers[N].forward.load(std::memory_order_acquire));
    if (!forward) return 0;
    const uintptr_t result = forward(self);
    recordCall(N, self, result);
    return result;
}

void* const kObservers[pad::kReaders] = {reinterpret_cast<void*>(&readerObserved<0>), reinterpret_cast<void*>(&readerObserved<1>),
                                         reinterpret_cast<void*>(&readerObserved<2>), reinterpret_cast<void*>(&readerObserved<3>)};
const char* const kReaderNames[pad::kReaders] = {"state-reader-FUN_12C3C20", "state-reader-FUN_12CAF80", "state-reader-FUN_12CADE0",
                                                 "state-reader-FUN_12CB320"};

// ---- relays (the store hook's layout: mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback]; original: jmp [trampoline]) ----------

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

void buildRelay(uint8_t* code, const void* gate, void* callback) noexcept {
    const uint8_t body[kRelayBytes] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x83, 0x38, 0, 0x74, 0x0E, 0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                       0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(code, body, sizeof(body));
    const uintptr_t gateAddress = reinterpret_cast<uintptr_t>(gate);
    const uintptr_t callbackAddress = reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code + 2, &gateAddress, 8);
    std::memcpy(code + 22, &callbackAddress, 8);
}

bool prepareRelay(void* trampoline, void* context) noexcept {
    auto& r = *static_cast<Reader*>(context);
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(r.relay + kOriginalLiteral, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(r.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) || !FlushInstructionCache(GetCurrentProcess(), r.relay, kRelayBytes)) return false;
    r.forward.store(address, std::memory_order_release);
    return true;
}

// Installs the four readers once: the identity (one refusal for all), then each reader's prologue pin. Each refusal is final and logged once.
void installReaders(uintptr_t base) {
    uint32_t stamp = 0;
    uint32_t size = 0;
    const bool identityRead = readIdentity(base, &stamp, &size);
    if (!identityRead || stamp != ecm::kExpectedTimestamp || size != ecm::kExpectedImageSize) {
        for (size_t i = 0; i < pad::kReaders; ++i) {
            g_readers[i].refused.store(true, std::memory_order_release);
            g_readers[i].name = kReaderNames[i];
            g_readers[i].status = "not build 332841 (PE timestamp or image size)";
        }
        Log::get().note("pad state probe: refused: not build 332841 (PE timestamp %u, image size %u); the probe stands down.",
                        static_cast<unsigned>(stamp), static_cast<unsigned>(size));
        return;
    }
    for (size_t i = 0; i < pad::kReaders; ++i) {
        Reader& r = g_readers[i];
        r.name = kReaderNames[i];
        const uintptr_t target = base + static_cast<uintptr_t>(pad::kReaderRva[i]);
        uint8_t got[sizeof(pad::kReaderPin)] = {};
        const bool readOk = readBytes(target, got, sizeof(got));
        const pad::PadInstall verdict = pad::padInstallDecision(identityRead, stamp, size, ecm::kExpectedTimestamp, ecm::kExpectedImageSize,
                                                                readOk ? got : nullptr, pad::kReaderPin, sizeof(got));
        if (verdict != pad::PadInstall::kInstall) {
            r.refused.store(true, std::memory_order_release);
            r.status = "prologue does not match build 332841";
            Log::get().note("pad state probe: refused: reader %s prologue is not build 332841's; that reader is not observed.", r.name);
            continue;
        }
        r.relay = allocateRelay(target);
        if (!r.relay) {
            r.refused.store(true, std::memory_order_release);
            r.status = "no relay within range";
            Log::get().note("pad state probe: refused: no relay within range for %s; that reader is not observed.", r.name);
            continue;
        }
        buildRelay(r.relay, &g_padGate, kObservers[i]);
        if (!r.hook.install(reinterpret_cast<void*>(target), r.relay, nullptr, r.name, &prepareRelay, &r)) {
            VirtualFree(r.relay, 0, MEM_RELEASE);
            r.relay = nullptr;
            r.refused.store(true, std::memory_order_release);
            r.status = "CodeHook refused (its own line says why)";
            Log::get().note("pad state probe: refused: CodeHook would not install %s; that reader is not observed.", r.name);
            continue;
        }
        r.ready.store(true, std::memory_order_release);
        r.status = "installed";
    }
    Log::get().note("pad state probe: readers %s=%s %s=%s %s=%s %s=%s (log only: no change to the station's behaviour).", g_readers[0].name,
                    g_readers[0].status, g_readers[1].name, g_readers[1].status, g_readers[2].name, g_readers[2].status, g_readers[3].name,
                    g_readers[3].status);
}

bool anyReady() noexcept {
    for (const Reader& r : g_readers) {
        if (r.ready.load(std::memory_order_acquire)) return true;
    }
    return false;
}

}  // namespace

void padStateSetOn(bool on) noexcept {
    if (on && !g_attempted) {
        g_attempted = true;
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        g_qpcFreq = static_cast<uint64_t>(f.QuadPart);
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (base) {
            installReaders(base);
        } else {
            Log::get().note("pad state probe: refused: the module base is unknown; the probe stands down.");
        }
    }
    g_padGate.store(on && anyReady() ? 1u : 0u, std::memory_order_release);
}

void padStateEvent(const char* what) noexcept { g_padEvent.store(what, std::memory_order_release); }

// The game's pad state, for the station's trigger (built v14.23). The latest r0 call on any object, and one object's r0 and r1 with their QPC times.
// Read-only: a copy under the spinlock.
void padStateLatestR0(PadObjState* out) noexcept {
    *out = PadObjState{};
    SpinLock guard;
    uint64_t best = 0;
    for (size_t i = 0; i < pad::kObjects; ++i) {
        const pad::PadObject& o = g_table.slots[i];
        if (!o.used || o.readerQpc[0] == 0 || o.readerQpc[0] <= best) continue;
        best = o.readerQpc[0];
        out->have = true;
        out->self = o.self;
        out->r0 = o.code[0];
        out->r0Qpc = o.readerQpc[0];
    }
}

void padStateObject(uintptr_t self, PadObjState* out) noexcept {
    *out = PadObjState{};
    out->self = self;
    SpinLock guard;
    for (size_t i = 0; i < pad::kObjects; ++i) {
        const pad::PadObject& o = g_table.slots[i];
        if (!o.used || o.self != self) continue;
        out->have = true;
        out->r0 = o.readerQpc[0] != 0 ? o.code[0] : -1;
        out->r0Qpc = o.readerQpc[0];
        out->r1 = o.readerQpc[1] != 0 ? o.code[1] : -1;
        out->r1Qpc = o.readerQpc[1];
        return;
    }
}

uint64_t padStateAgeMs(uint64_t qpc) noexcept {
    if (qpc == 0 || g_qpcFreq == 0) return UINT64_MAX;
    const uint64_t now = padQpcNow();
    if (now <= qpc) return 0;
    return (now - qpc) * 1000u / g_qpcFreq;
}

uint64_t padStateNowQpc() noexcept { return padQpcNow(); }

// The tick (built v14.23). Writes the change lines and the seal or release line for the OWN pad only, and the 30 s summary. The own pad is named by
// the caller (the station's GameTrack); with no own pad, nothing per object is written.
void padStateTick(uint64_t nowMs, uintptr_t own) noexcept {
    if (!g_attempted) return;
    const char* ev = g_padEvent.exchange(nullptr, std::memory_order_acq_rel);
    if (ev != nullptr) {
        static char evLine[512];
        {
            SpinLock guard;
            pad::formatPadEvent(evLine, sizeof(evLine), ev, g_table, own);
        }
        Log::get().note("%s", evLine);
    }
    pad::PadObject snap{};
    bool have = false;
    double elapsedS = 0.0;
    {
        SpinLock guard;
        const uint64_t now = padQpcNow();
        if (own != 0) {
            for (size_t i = 0; i < pad::kObjects; ++i) {
                pad::PadObject& o = g_table.slots[i];
                if (!o.used || o.self != own) continue;
                const bool first = !o.announced;
                if (!first && !pad::padChanged(o)) break;
                snap = o;   // the copy keeps the state as it was: the codes and reading last printed, the calls since
                have = true;
                const uint64_t base = o.lineQpc != 0 ? o.lineQpc : o.lastQpc;
                elapsedS = qpcSeconds(now > base ? now - base : 0);
                o.announced = true;
                for (size_t k = 0; k < pad::kReaders; ++k) {
                    o.lineCode[k] = o.code[k];
                    o.calledSinceLine[k] = false;
                }
                o.lineReading = o.reading;
                o.callsSinceLine = 0;
                o.lineQpc = now;
                break;
            }
        }
    }
    if (have) {
        const pad::ChangeLine verdict = pad::padAllowChangeLine(g_table);
        char line[512];
        if (verdict == pad::ChangeLine::kWrite) {
            pad::formatPadChange(line, sizeof(line), snap, elapsedS);
            Log::get().note("%s", line);
        } else if (verdict == pad::ChangeLine::kSuppressedFirst) {
            Log::get().note("pad state: %u change lines logged this session; further changes are counted in the 30 s summary, not logged.",
                            static_cast<unsigned>(pad::kChangeLineCap));
        }
    }
    if (g_summaryStartMs == 0) g_summaryStartMs = nowMs;
    if (nowMs - g_summaryStartMs >= 30000) {
        char line[512];
        const double windowS = static_cast<double>(nowMs - g_summaryStartMs) / 1000.0;
        bool anyCalls = false;
        {
            SpinLock guard;
            for (const uint64_t c : g_table.readerCalls) anyCalls = anyCalls || c != 0;
            pad::formatPadSummary(line, sizeof(line), g_table, windowS);
            for (uint64_t& c : g_table.readerCalls) c = 0;
            g_table.maxProbes = 0;
            g_table.suppressed = 0;
        }
        if (anyCalls) Log::get().note("%s", line);   // built v14.31: a window in which the game called no docking reader writes no line
        g_summaryStartMs = nowMs;
    }
}

}  // namespace edvr
