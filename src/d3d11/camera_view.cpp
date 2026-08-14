// GENERATED from src/d3d11/camera_view.cpp in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 93f25cf02880460c]
#include "camera_view.h"

#include <windows.h>
#include <tlhelp32.h>

#include <string>
#include <vector>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"

namespace edvr {
namespace {

constexpr size_t kPage = 4096;

// Enough that a busy process never trips it, small enough that a wrong region
// list does.
//
// A page can be freed between VirtualQuery saying it is committed and this
// reading it, which is normal and unavoidable in a live process -- a measured
// scan absorbed 23 of them. The budget is not there to tolerate that; it is
// there so a scan walking nonsense stops instead of faulting forever.
constexpr uint64_t kMaxFaults = 4096;

// Per-READ faults, which are a different thing from scan faults and need a much
// smaller budget. A scan walks unknown memory once and expects a few misses; a
// read touches one known address every frame forever, so a handful of failures
// means that address is gone, not that we were unlucky.
constexpr uint64_t kMaxReadFaults = 8;

struct Region {
    const uint8_t* base;
    size_t         size;
};

struct State {
    bool      track = true;
    size_t    ordinal = 11;          // 6ad.8b, stable across two launches
    uintptr_t typeOffset = 0x4D71C50;
    size_t    valueOffset = 0x10;
    uint32_t  plausibleMax = 7;
    size_t    bytesPerFrame = 64u << 20;

    const uint8_t* typePtr = nullptr;
    std::vector<Region> regions;
    size_t    regionIndex = 0;
    size_t    regionOffset = 0;
    uint64_t  bytesScanned = 0;
    uint64_t  faults = 0;
    uint64_t  regionsGone = 0;

    bool      scanning = false;
    bool      scanned = false;
    std::vector<const uint8_t*> records;

    int       lastView = -1;
    bool      failNoted = false;
    // Once the record has proven to be somebody else's, STAY at "do not know".
    //
    // The old code only suppressed the log line: the next in-range garbage was
    // reported as authoritative again, so a reused block flickered between
    // "correct" and "wrong" with the wrong readings silently winning. Reuse is
    // not a transient -- the allocation is gone -- so the answer is gone until
    // a fresh scan, and the gate falls back to counting keypresses.
    bool      lost = false;
    uint64_t  readFaults = 0;
};

State g_s;

// Thread stacks, excluded from the scan.
//
// A stack holds whatever a call frame happened to leave there, so it supplies
// matches that look exactly like real ones and are gone by the next frame.
// Measured: every survivor of an early scan came from a stack.
void collectStacks(std::vector<Region>* out) {
    out->clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD self = GetCurrentProcessId();
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != self) continue;
            HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!th) continue;
            // The TEB address, via NtQueryInformationThread, resolved by name so
            // a missing export degrades to "no stacks excluded" rather than
            // failing to load the DLL.
            typedef LONG(NTAPI * PFN_NtQIT)(HANDLE, int, PVOID, ULONG, PULONG);
            static PFN_NtQIT fn = reinterpret_cast<PFN_NtQIT>(reinterpret_cast<void*>(
                GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                               "NtQueryInformationThread")));
            if (fn) {
                struct { PVOID exitStatus; PVOID teb; PVOID a, b, c, d; } tbi{};
                if (fn(th, 0 /*ThreadBasicInformation*/, &tbi, sizeof(tbi), nullptr) == 0 &&
                    tbi.teb) {
                    guarded("camera_view/teb", [&] {
                        // x64 TEB: NT_TIB is first, StackBase at +0x08,
                        // StackLimit at +0x10.
                        const uint8_t* teb = static_cast<const uint8_t*>(tbi.teb);
                        const uint8_t* hi = *reinterpret_cast<const uint8_t* const*>(teb + 0x08);
                        const uint8_t* lo = *reinterpret_cast<const uint8_t* const*>(teb + 0x10);
                        if (lo && hi && hi > lo) {
                            out->push_back({lo, static_cast<size_t>(hi - lo)});
                        }
                    });
                }
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

bool overlapsStack(const std::vector<Region>& stacks, const uint8_t* base, size_t size) {
    for (const Region& s : stacks) {
        if (base < s.base + s.size && s.base < base + size) return true;
    }
    return false;
}

// MEM_PRIVATE only, so mapped images are skipped and the game's code is never
// read. Committed, readable and not guard pages, because anything else faults
// by definition rather than by accident.
void collectRegions() {
    std::vector<Region> stacks;
    collectStacks(&stacks);

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uint8_t* p = static_cast<const uint8_t*>(si.lpMinimumApplicationAddress);
    const uint8_t* end = static_cast<const uint8_t*>(si.lpMaximumApplicationAddress);
    g_s.regions.clear();
    uint64_t total = 0;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        const uint8_t* next = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        const bool usable =
            mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_WRITECOPY) &&
            (mbi.Protect & PAGE_GUARD) == 0;
        if (usable && mbi.RegionSize >= kPage &&
            !overlapsStack(stacks, static_cast<const uint8_t*>(mbi.BaseAddress),
                           mbi.RegionSize)) {
            g_s.regions.push_back({static_cast<const uint8_t*>(mbi.BaseAddress),
                                   mbi.RegionSize});
            total += mbi.RegionSize;
        }
        if (next <= p) break;
        p = next;
    }
    // The megabyte figure is the coverage this result is worth. A few hundred
    // rather than several thousand means the scan ran before the game had
    // allocated, and a negative from it says nothing at all -- which is the
    // failure that cost two attempts before the trigger moved to the first
    // panel frame.
    Log::get().note("camera view: scanning %zu region(s), %.0f MB, %zu thread "
                    "stack(s) excluded.",
                    g_s.regions.size(), (double)total / (1024.0 * 1024.0),
                    stacks.size());
}

// One frame's slice of the walk. Returns true when the whole address space has
// been covered.
//
// Spread over frames rather than done at once: 11 GB in a single call stalls
// the headset for several seconds, which from inside one is indistinguishable
// from a hang.
bool scanSlice() {
    size_t budget = g_s.bytesPerFrame;
    while (budget > 0 && g_s.regionIndex < g_s.regions.size()) {
        const Region& r = g_s.regions[g_s.regionIndex];
        if (g_s.regionOffset >= r.size) {
            ++g_s.regionIndex;
            g_s.regionOffset = 0;
            continue;
        }
        const uint8_t* page = r.base + g_s.regionOffset;
        const bool ok = guarded("camera_view/page", [&] {
            const uint8_t* const* q = reinterpret_cast<const uint8_t* const*>(page);
            for (size_t i = 0; i < kPage / 8; ++i) {
                if (q[i] == g_s.typePtr) {
                    g_s.records.push_back(reinterpret_cast<const uint8_t*>(&q[i]));
                }
            }
        });
        if (!ok) {
            // The region went away under us. Skip the rest of it rather than
            // faulting page by page through something that no longer exists.
            if (++g_s.faults > kMaxFaults) {
                Log::get().note("camera view: giving up after %llu faults -- the "
                                "region list is not describing this process.",
                                (unsigned long long)g_s.faults);
                g_s.regionIndex = g_s.regions.size();
                break;
            }
            ++g_s.regionsGone;
            ++g_s.regionIndex;
            g_s.regionOffset = 0;
            continue;
        }
        g_s.regionOffset += kPage;
        g_s.bytesScanned += kPage;
        budget = budget > kPage ? budget - kPage : 0;
    }
    return g_s.regionIndex >= g_s.regions.size();
}

// The value in a record, or 0xFFFFFFFF if it cannot be read or is not a record
// any more.
//
// THE ANCHOR IS RE-CHECKED EVERY TIME. A record is identified by its first
// qword being the type pointer, and that is what the scan matched -- but the
// address was then trusted for the rest of the session. Heap memory gets
// recycled. Once that allocation belongs to something else, the "view index" is
// whatever the new owner put there, and small integers are the most common
// thing in a heap, so the <= 7 plausibility filter passes it happily. Re-reading
// one qword per frame is nothing next to believing a number from a freed block.
uint32_t recordValue(size_t i) {
    uint32_t v = 0xFFFFFFFFu;
    // A budget, because a decommitted page faults on EVERY read. Without one
    // this is ~90 access violations and ~90 formatted log lines per second for
    // the rest of the session -- the guard absorbing them correctly and the log
    // becoming unreadable, which is the instrument destroying the evidence.
    if (g_s.readFaults > kMaxReadFaults) return v;
    const bool ok = guarded("camera_view/read", [&] {
        const uint8_t* const* anchor =
            reinterpret_cast<const uint8_t* const*>(g_s.records[i]);
        if (*anchor != g_s.typePtr) return;   // not a record of ours any more
        v = *reinterpret_cast<const uint32_t*>(g_s.records[i] + g_s.valueOffset);
    });
    if (!ok) ++g_s.readFaults;
    return v;
}

void finishScan() {
    g_s.scanning = false;
    g_s.scanned = true;

    if (g_s.records.empty()) {
        Log::get().note(
            "camera view: no records of that type found over %.0f MB, so the view "
            "index cannot be read and the gate will count keypresses instead. "
            "Most likely the game updated and d3d11.camera_index_type_offset "
            "(0x%llX) no longer points at the right thing.",
            (double)g_s.bytesScanned / (1024.0 * 1024.0),
            (unsigned long long)g_s.typeOffset);
        return;
    }
    if (g_s.ordinal >= g_s.records.size()) {
        Log::get().note("camera view: only %zu record(s), so ordinal %zu does not "
                        "exist. The array is not the one this was measured on -- "
                        "most likely the game updated and "
                        "d3d11.camera_index_type_offset moved. Falling back to "
                        "counting keypresses.",
                        g_s.records.size(), g_s.ordinal);
        return;
    }
    const uint32_t v = recordValue(g_s.ordinal);
    Log::get().note("camera view: %zu record(s) over %.0f MB; tracking ordinal %zu, "
                    "which reads %u%s.",
                    g_s.records.size(),
                    (double)g_s.bytesScanned / (1024.0 * 1024.0), g_s.ordinal, v,
                    v <= g_s.plausibleMax
                        ? " -- a plausible view"
                        : " -- OUTSIDE the plausible range, so this is not the "
                          "index and the gate will count keypresses instead");
}

}  // namespace

void cameraViewConfigure() {
    Config& cfg = Config::get();
    g_s.track = cfg.getBool("d3d11.camera_index_track", true);
    g_s.ordinal = static_cast<size_t>(
        cfg.getIntInRange("d3d11.camera_index_ordinal", 11, 0, 4095));
    g_s.valueOffset = static_cast<size_t>(
        cfg.getIntInRange("d3d11.camera_index_value_offset", 0x10, 0, 0x1000));
    // Bounded: -1 wrapped to 0xFFFFFFFF and disabled the only filter standing
    // between heap garbage and a view index the gate believes.
    g_s.plausibleMax = static_cast<uint32_t>(
        cfg.getIntInRange("d3d11.camera_index_plausible_max", 7, 0, 1023));
    // Bounded: 0 meant the walk advanced nothing and never finished, silently,
    // and a negative scanned the whole address space in a single frame -- the
    // multi-second stall the slicing exists to prevent.
    g_s.bytesPerFrame = static_cast<size_t>(
        cfg.getIntInRange("d3d11.camera_index_mb_per_frame", 64, 1, 256)) << 20;
    // Hex, because that is how an offset into an executable is written
    // everywhere else it appears -- in the log, in EVIDENCE, in a debugger.
    g_s.typeOffset = static_cast<uintptr_t>(strtoull(
        cfg.getString("d3d11.camera_index_type_offset", "0x4D71C50").c_str(),
        nullptr, 0));

    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    g_s.typePtr = base ? base + g_s.typeOffset : nullptr;
}

void cameraViewRequestScan() {
    if (!g_s.track || g_s.scanned || g_s.scanning || !g_s.typePtr) return;
    g_s.scanning = true;
    g_s.records.clear();
    g_s.regionIndex = 0;
    g_s.regionOffset = 0;
    g_s.bytesScanned = 0;
    g_s.faults = 0;
    collectRegions();
}

void cameraViewTick() {
    if (!g_s.scanning) return;
    if (scanSlice()) finishScan();
}

int cameraViewCurrent() {
    if (!g_s.track || !g_s.scanned || g_s.lost) return -1;
    if (g_s.ordinal >= g_s.records.size()) return -1;
    const uint32_t v = recordValue(g_s.ordinal);
    // Outside the range means the record has been reused and the answer would
    // be a guess. -1 says "do not know", which the caller treats as "fall
    // back", rather than a number that happens to be wrong.
    const int out = v <= g_s.plausibleMax ? static_cast<int>(v) : -1;
    if (out != g_s.lastView) {
        g_s.lastView = out;
        if (out >= 0) {
            Log::get().note("camera view is %d (ordinal %zu, read from the game)",
                            out, g_s.ordinal);
        } else if (!g_s.failNoted) {
            g_s.failNoted = true;
            g_s.lost = true;
            Log::get().note("camera view: ordinal %zu no longer reads as a camera "
                            "record, so that heap block has been reused. LATCHED "
                            "to \"do not know\" -- the allocation is gone, and a "
                            "later in-range value from whatever owns it now would "
                            "be garbage that happens to look plausible. Counting "
                            "keypresses from here.", g_s.ordinal);
        }
    }
    return out;
}

}  // namespace edvr
