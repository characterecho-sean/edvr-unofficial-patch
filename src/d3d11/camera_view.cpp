// GENERATED from src/d3d11/camera_view.cpp in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 4e2218a48cef0c6a]
#include "camera_view.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
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

// Four attempts, a minute apart. Enough to cover a slow load or a player who
// reaches the surface late; few enough that a genuinely wrong anchor -- the
// game-update case -- stops rather than walking the heap every minute forever.
constexpr uint32_t kMaxAttempts = 4;
constexpr uint32_t kRetryCooldown = 3600;

// Rescans after the array has MOVED, which is a different budget from attempts
// to find it in the first place. Re-finding is cheap to justify -- the array
// demonstrably exists, we had it a moment ago -- so this is generous. It is
// bounded at all only so that a pathological session cannot walk the heap
// forever, and briefly delayed so a move during a mode change is not chased
// several times over.
constexpr uint32_t kMaxRescans = 16;

// The camera settings records are 0x18 bytes and lie back to back (6ad.7a,
// 6ad.8a: stride 0x18, no gaps, 19 of them). That structure is what tells the
// array apart from unrelated objects of the same type scattered elsewhere.
constexpr size_t kStride = 0x18;
constexpr uint32_t kRescanCooldown = 240;

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

    // The record finally chosen, rather than an index into `records`.
    //
    // Indexing the match list globally is what broke: the ordinal is a position
    // WITHIN the camera settings array (6ad.7a), and the match list is every
    // object of that type anywhere in the heap. 6ad.8a measured the array at 19
    // contiguous records; sessions since have matched 17, 31 and 32 objects, so
    // "the 12th match in address order" stopped being "the 12th record of the
    // array" the moment anything else of that type existed.
    const uint8_t* chosen = nullptr;

    bool      scanning = false;
    bool      scanned = false;
    // RETRY, because "the right moment to scan" is a judgement and it has
    // already been wrong twice: once at DLL-attach with 127 MB allocated, and
    // once four seconds after launch with 5 GB of an eventual 11 GB. Both times
    // the scan latched its failure and never looked again, so a single bad
    // guess about timing disabled the feature for the whole session.
    //
    // A scan that finds nothing usable is now an attempt, not a verdict.
    bool      usable = false;
    uint32_t  attempts = 0;
    uint32_t  cooldown = 0;
    bool      exhaustedNoted = false;
    std::vector<const uint8_t*> records;

    int       lastView = -1;
    bool      failNoted = false;
    // The record has proven to be somebody else's, so RESCAN -- do not latch.
    //
    // Three positions have been held here and only the third is right. Trusting
    // the address forever let reused heap report garbage as a camera view.
    // Latching to "do not know" stopped that, and killed the feature for the
    // session the first time the game moved its array -- measured, 40 seconds
    // into a working session: "ordinal 11 no longer reads as a camera record".
    //
    // The anchor check is what makes the third option available. It does not
    // merely suspect the address is stale, it PROVES it: the first qword is no
    // longer the type pointer, so this is not the record. The array itself
    // still exists somewhere, and a scan is exactly the thing that finds it.
    bool      needRescan = false;
    uint32_t  rescans = 0;
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
    Log::get().note("camera view: attempt %u of %u -- scanning %zu region(s), "
                    "%.0f MB, %zu thread stack(s) excluded. If that megabyte "
                    "figure is a few thousand rather than around ten, this ran "
                    "before the game finished loading and will be retried.",
                    g_s.attempts + 1, kMaxAttempts, g_s.regions.size(),
                    (double)total / (1024.0 * 1024.0), stacks.size());
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
uint32_t recordValue(const uint8_t* rec) {
    uint32_t v = 0xFFFFFFFFu;
    // A budget, because a decommitted page faults on EVERY read. Without one
    // this is ~90 access violations and ~90 formatted log lines per second for
    // the rest of the session -- the guard absorbing them correctly and the log
    // becoming unreadable, which is the instrument destroying the evidence.
    if (g_s.readFaults > kMaxReadFaults) return v;
    const bool ok = guarded("camera_view/read", [&] {
        const uint8_t* const* anchor =
            reinterpret_cast<const uint8_t* const*>(rec);
        if (*anchor != g_s.typePtr) return;   // not a record of ours any more
        v = *reinterpret_cast<const uint32_t*>(rec + g_s.valueOffset);
    });
    if (!ok) ++g_s.readFaults;
    return v;
}

// Called when a scan completes. Picks the camera settings ARRAY out of the
// matches, then the ordinal within it.
//
// WHY THIS IS NOT JUST records[ordinal]
//
// The ordinal is a position within one array (6ad.7a: 0x18-byte records, the
// index at +0x10; 6ad.8a: 19 of them, contiguous, no gaps). The match list is
// every object of that type anywhere in the process. Those were the same thing
// for exactly as long as nothing else of that type existed -- 17 matches twice,
// which is what "the ordinal is stable" was measured on.
//
// It is not the same thing now. Sessions since have matched 17, 31 and 32, and
// at 32 the value at global position 11 was 2210427397: not a view index, just
// whatever the twelfth object in address order happened to hold. Indexing
// across a concatenation of unrelated objects gives a number every time and a
// correct one by luck.
//
// So: group by the structure the array actually has, and require the answer to
// be unambiguous. Two candidate arrays is a reason to say "do not know", not a
// reason to pick one -- picking is what produced the wrong answer above.
void finishScan() {
    g_s.scanning = false;
    g_s.scanned = true;
    ++g_s.attempts;
    g_s.cooldown = kRetryCooldown;
    g_s.chosen = nullptr;
    const char* retry =
        g_s.attempts < kMaxAttempts
            ? " Trying again in about a minute, in case this ran before the game "
              "had finished loading."
            : " That was the last attempt; which camera preset you are on will "
              "be unknown for the rest of this session.";

    if (g_s.records.empty()) {
        Log::get().note(
            "camera view: no records of that type found over %.0f MB. Most "
            "likely the game updated and d3d11.camera_index_type_offset "
            "(0x%llX) no longer points at the right thing.",
            (double)g_s.bytesScanned / (1024.0 * 1024.0),
            (unsigned long long)g_s.typeOffset);
        Log::get().note("camera view:%s", retry);
        return;
    }

    // Runs of consecutive records at the array's own stride.
    std::sort(g_s.records.begin(), g_s.records.end());
    struct Run { const uint8_t* base; size_t count; };
    std::vector<Run> runs;
    for (size_t i = 0; i < g_s.records.size(); ++i) {
        if (!runs.empty() &&
            g_s.records[i] == runs.back().base + runs.back().count * kStride) {
            ++runs.back().count;
        } else {
            runs.push_back({g_s.records[i], 1});
        }
    }

    // A candidate is a run long enough to hold the ordinal, whose value there
    // looks like a view. Both halves matter: length alone would accept any
    // long stretch, and plausibility alone would accept a lone small integer.
    std::vector<size_t> candidates;
    for (size_t r = 0; r < runs.size(); ++r) {
        if (runs[r].count <= g_s.ordinal) continue;
        const uint32_t v = recordValue(runs[r].base + g_s.ordinal * kStride);
        if (v <= g_s.plausibleMax) candidates.push_back(r);
    }

    // The run structure, always, because it is what makes the next session's
    // log readable rather than a bare count. Capped so a pathological heap
    // cannot fill the file.
    Log::get().note("camera view: %zu record(s) over %.0f MB in %zu run(s) of "
                    "stride 0x%zX; %zu run(s) long enough to hold ordinal %zu "
                    "with a plausible value there.",
                    g_s.records.size(),
                    (double)g_s.bytesScanned / (1024.0 * 1024.0), runs.size(),
                    kStride, candidates.size(), g_s.ordinal);
    for (size_t r = 0; r < runs.size() && r < 8; ++r) {
        const uint32_t v = runs[r].count > g_s.ordinal
                               ? recordValue(runs[r].base + g_s.ordinal * kStride)
                               : 0xFFFFFFFFu;
        Log::get().note("camera view:   run %zu at %p, %zu record(s)%s",
                        r, (const void*)runs[r].base, runs[r].count,
                        runs[r].count > g_s.ordinal
                            ? (v <= g_s.plausibleMax ? " -- ordinal reads a plausible view"
                                                     : " -- ordinal reads something else")
                            : " -- too short for the ordinal");
    }

    if (candidates.size() == 1) {
        g_s.chosen = runs[candidates[0]].base + g_s.ordinal * kStride;
        g_s.usable = true;
        g_s.cooldown = 0;
        Log::get().note("camera view: tracking run %zu, ordinal %zu, which reads "
                        "%u -- a plausible view.",
                        candidates[0], g_s.ordinal, recordValue(g_s.chosen));
        return;
    }
    if (candidates.empty()) {
        Log::get().note("camera view: no run holds a plausible view at ordinal "
                        "%zu, so the camera settings array was not among what "
                        "was found.", g_s.ordinal);
    } else {
        // Deliberately not a guess. Choosing between equally-qualified arrays
        // is what put 2210427397 in a view index in the first place.
        Log::get().note("camera view: %zu runs qualify equally, so which one is "
                        "the camera settings cannot be told apart from here. "
                        "Refusing rather than picking; the run list above is "
                        "what a fix would need.", candidates.size());
    }
    Log::get().note("camera view:%s", retry);
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
    if (!g_s.track || g_s.usable || g_s.scanning || !g_s.typePtr) return;
    if (g_s.cooldown > 0) return;
    // A rescan after a move does not spend the find-it-first-time budget.
    if (g_s.needRescan) {
        if (g_s.rescans >= kMaxRescans) return;
        ++g_s.rescans;
        g_s.needRescan = false;
        g_s.attempts = 0;      // this is a re-find, not another failed search
    } else if (g_s.attempts >= kMaxAttempts) {
        // Once, so the log distinguishes "gave up" from "never asked". The last
        // flight could not tell those apart and neither could I.
        if (!g_s.exhaustedNoted) {
            g_s.exhaustedNoted = true;
            Log::get().note("camera view: %u attempts made and none found the "
                            "camera settings, so which camera preset you are on "
                            "is unknown for the rest of this session. If the "
                            "game has updated, d3d11.camera_index_type_offset "
                            "needs re-measuring.", g_s.attempts);
        }
        return;
    }
    g_s.scanning = true;
    g_s.chosen = nullptr;
    g_s.records.clear();
    g_s.regionIndex = 0;
    g_s.regionOffset = 0;
    g_s.bytesScanned = 0;
    g_s.faults = 0;
    collectRegions();
}

void cameraViewTick() {
    if (g_s.cooldown > 0) --g_s.cooldown;
    if (!g_s.scanning) return;
    if (scanSlice()) finishScan();
}

int cameraViewCurrent() {
    if (!g_s.track || !g_s.usable || !g_s.chosen) return -1;
    const uint32_t v = recordValue(g_s.chosen);
    // Outside the range means the record has been reused and the answer would
    // be a guess. -1 says "do not know", which the caller treats as "fall
    // back", rather than a number that happens to be wrong.
    const int out = v <= g_s.plausibleMax ? static_cast<int>(v) : -1;
    if (out != g_s.lastView) {
        g_s.lastView = out;
        if (out >= 0) {
            Log::get().note("camera view is %d (ordinal %zu, read from the game)",
                            out, g_s.ordinal);
        } else {
            // Not a latch: the array moved, so go and find it again. The
            // anchor check proved this address is no longer a record of ours,
            // which also means a scan can tell the real one from garbage.
            g_s.usable = false;
            g_s.chosen = nullptr;
            g_s.needRescan = true;
            g_s.cooldown = kRescanCooldown;
            if (!g_s.failNoted) {
                g_s.failNoted = true;
                Log::get().note("camera view: ordinal %zu no longer reads as a "
                                "camera record, so the game has moved its camera "
                                "settings. Scanning again shortly to find where "
                                "they went; the offset will not engage until it "
                                "does. This is expected occasionally and is not "
                                "a fault.", g_s.ordinal);
            }
        }
    }
    return out;
}

}  // namespace edvr
