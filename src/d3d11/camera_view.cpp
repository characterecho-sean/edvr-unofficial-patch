// GENERATED from src/d3d11/camera_view.cpp in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 4c9f9e38755215c8]
#include "camera_view.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
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

// Empty slots the grouping will bridge rather than end a run on. The reasoning,
// and the field evidence for it, is on cameraViewGroupRuns in the header.
//
// Two, not more. One is what has been measured; two gives the same shape a
// little room without letting unrelated objects a few strides apart be swept
// into one array.
constexpr size_t kMaxGap = 2;

// Consecutive frames the chosen record may fail to read before this concludes
// the array has actually MOVED.
//
// Without it, any single bad read spent a rescan -- and a rescan is not cheap:
// it walks ten gigabytes, and the trigger that starts one may be minutes away.
// The same user's logs show what that costs, twice, with the array never having
// moved at all: every scan of a session found it at the same address, and the
// only thing that had changed was that the records go briefly quiet while the
// game rebuilds them. Two seconds of holding still is the difference between
// riding that out and losing the feature for five minutes.
constexpr uint32_t kMoveGrace = 120;

// How long a run that is LONG ENOUGH but reads wrong is kept under observation
// before the scan is written off.
//
// The candidate test samples the ordinal once, at whatever instant the scan
// happens to finish, and treats that one sample as a property of the run. It is
// not: the slot holds a non-view value while the game changes mode, which is
// exactly the moment a player triggers a scan by walking about. Measured three
// times in one day, twice on the developer's machine and once on a reporter's --
// the same run rejected as "reads something else" and accepted a minute later.
//
// Ten seconds is longer than any mode change observed (25-86 frames, 6ac.6c)
// and short enough that a run which is genuinely not the camera settings is not
// watched for a meaningful part of a session.
constexpr uint32_t kProbeWindow = 600;

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

    // The one run that was long enough for the ordinal but did not read as a
    // view when the scan sampled it. Kept and re-read rather than discarded --
    // see kProbeWindow. Only ever set when there is exactly ONE such run, so
    // this cannot become the guess between equals that the refusal exists to
    // prevent.
    const uint8_t* provisional = nullptr;
    uint32_t  provisionalFrames = 0;

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
    bool      sawGameplay = false;
    // Consecutive frames the chosen record has not read as a record. Reset by
    // any good read, so this counts one episode rather than a session's total.
    uint32_t  badReads = 0;
    // Faults on the CURRENT record, reset whenever a new one is chosen.
    //
    // It never reset, and finishScan's own candidate probes charged it -- so
    // nine faults across an entire session, from any cause, permanently
    // disabled candidate selection and guaranteed that every one of the 16
    // rescans would fail. A budget that outlives the thing it is budgeting for
    // is not a budget, it is a countdown to the feature switching itself off.
    uint64_t  readFaults = 0;
    bool      readFaultsNoted = false;
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
    if (g_s.readFaults > kMaxReadFaults) {
        if (!g_s.readFaultsNoted) {
            g_s.readFaultsNoted = true;
            Log::get().note("camera view: %llu faults reading the chosen record, "
                            "so it is no longer being read. The page it lives on "
                            "has probably gone; a rescan will pick it up again.",
                            (unsigned long long)g_s.readFaults);
        }
        return v;
    }
    const bool ok = guarded("camera_view/read", [&] {
        const uint8_t* const* anchor =
            reinterpret_cast<const uint8_t* const*>(rec);
        if (*anchor != g_s.typePtr) return;   // not a record of ours any more
        v = *reinterpret_cast<const uint32_t*>(rec + g_s.valueOffset);
    });
    if (!ok) ++g_s.readFaults;
    return v;
}

// Is there a record at this address AT ALL?
//
// Distinguishes an EMPTY slot in the array from one holding a value we do not
// recognise, and those are opposite situations. An empty slot means the array
// has been found and the game has not filled it yet -- wait. A filled slot
// holding something implausible means this is probably not the array -- look
// elsewhere. recordValue answers 0xFFFFFFFF to both.
bool slotOccupied(const uint8_t* rec) {
    bool ok = false;
    guarded("camera_view/anchor", [&] {
        const uint8_t* const* anchor = reinterpret_cast<const uint8_t* const*>(rec);
        ok = (*anchor == g_s.typePtr);
    });
    return ok;
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

    // Runs of records at the array's own stride, bridging the occasional empty
    // slot -- see cameraViewGroupRuns for why the bridging is load-bearing.
    std::sort(g_s.records.begin(), g_s.records.end());
    const std::vector<CameraViewRun> runs =
        cameraViewGroupRuns(g_s.records, kStride, kMaxGap);

    // A candidate is a run long enough to hold the ordinal, whose value there
    // looks like a view. Both halves matter: length alone would accept any
    // long stretch, and plausibility alone would accept a lone small integer.
    //
    // SLOTS, not filled records: the ordinal is a position in the array, and a
    // hole earlier in the run does not move it.
    std::vector<size_t> candidates;
    std::vector<size_t> longEnough;
    for (size_t r = 0; r < runs.size(); ++r) {
        if (runs[r].slots <= g_s.ordinal) continue;
        longEnough.push_back(r);
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
    // Slots AND filled, because they came apart in the field and the log said
    // only "record(s)" -- which read as a short array rather than as a whole one
    // with a hole in it, and that is the difference between "the anchor is
    // wrong" and "the grouping is wrong".
    for (size_t r = 0; r < runs.size() && r < 8; ++r) {
        const uint32_t v = runs[r].slots > g_s.ordinal
                               ? recordValue(runs[r].base + g_s.ordinal * kStride)
                               : 0xFFFFFFFFu;
        Log::get().note("camera view:   run %zu at %p, %zu slot(s), %zu filled%s",
                        r, (const void*)runs[r].base, runs[r].slots,
                        runs[r].present,
                        runs[r].slots > g_s.ordinal
                            ? (v <= g_s.plausibleMax ? " -- ordinal reads a plausible view"
                                                     : " -- ordinal reads something else")
                            : " -- too short for the ordinal");
    }

    // WHEN NOTHING FITS, PRINT WHAT WAS THERE.
    //
    // The summary above says the ordinal did not land on a usable value; it does
    // not say what any of the other slots held, and that is the only information
    // that can settle where the view index actually lives. Three layouts have now
    // been recorded for the same twelve records -- one run of 12 (works), one run
    // of 14 with 12 filled (ordinal reads something else), and two runs of 10 + 2
    // separated by a five-slot hole (ordinal lands in the hole). "Index 11 from
    // the base" and "the last of the twelve" are the same slot in the first
    // layout and different slots in the third, so a log that reports only the
    // ordinal cannot distinguish the anchor being wrong from the grouping being
    // wrong.
    //
    // IT WALKS THE RECORD LIST, NOT THE RUNS, and that is the whole point of it.
    //
    // The first version walked runs, and runs are exactly what is unreliable
    // here: a record that has moved out of contiguity is not in any run, so the
    // records most worth seeing were the ones it could not show. 6am's capture
    // has ten records inside a run and two somewhere else, and the two missing
    // addresses are precisely the evidence needed -- if the twelfth RECORD in
    // address order holds a plausible view wherever it sits, then the ordinal
    // counts records rather than slots, and that is a different fix from a wider
    // gap tolerance.
    //
    // So: every record the scan found, in address order, with its offset in
    // strides from the first. Holes are printed too, because a hole is evidence
    // -- it says the records moved rather than that there are fewer of them --
    // but the walk is anchored on records and merely notes the gaps between
    // them, which is the reverse of what it did before.
    //
    // FOUND-THEN and READS-NOW are both shown, because they disagree and the
    // disagreement is real. The 14:14 capture reported twelve records present
    // and only ten still readable seconds later: `present` is counted during the
    // scan and the value is re-read at this moment, and in between the array was
    // losing entries. Printing one number would have hidden the very behaviour
    // that explains the failure.
    if (candidates.empty() && !g_s.records.empty()) {
        Log::get().note(
            "camera view: no run fits, so here is every record the scan found, in "
            "address order. FOUND is what the scan saw; READS is what the same "
            "address holds right now, and the two differing means the array is "
            "losing entries as we look. The one that holds your camera preset is "
            "the one whose READS value CHANGES when you cycle cameras -- this is "
            "printed again on each retry, about a minute apart, so switching "
            "preset in between is what identifies it.");

        const uint8_t* first = g_s.records.front();
        size_t shown = 0, stillReading = 0;
        for (size_t i = 0; i < g_s.records.size() && shown < 40; ++i) {
            const uint8_t* rec = g_s.records[i];
            // Distance from the first record, in strides. This is the number the
            // ordinal is currently compared against, so printing it beside the
            // record index is what makes the two readings distinguishable at a
            // glance: where they differ, the records are not contiguous.
            const size_t slotIdx = static_cast<size_t>(rec - first) / kStride;
            if (i > 0) {
                const size_t prev =
                    static_cast<size_t>(g_s.records[i - 1] - first) / kStride;
                if (slotIdx > prev + 1) {
                    Log::get().note(
                        "camera view:   ... %zu empty slot(s) here ...",
                        slotIdx - prev - 1);
                }
            }
            const uint32_t v = recordValue(rec);
            char reads[48];
            if (v == 0xFFFFFFFFu) {
                snprintf(reads, sizeof(reads), "READS gone");
            } else {
                snprintf(reads, sizeof(reads), "READS %u%s", v,
                         v <= g_s.plausibleMax ? " -- could be a view" : "");
            }
            Log::get().note(
                "camera view:   record %2zu (slot %2zu) at %p  %s%s%s", i,
                slotIdx, (const void*)rec, reads,
                i == g_s.ordinal ? "   <- ordinal counted over RECORDS" : "",
                slotIdx == g_s.ordinal ? "   <- ordinal counted over SLOTS" : "");
            if (v != 0xFFFFFFFFu) ++stillReading;
            ++shown;
        }
        if (g_s.records.size() > shown) {
            Log::get().note("camera view:   ... and %zu more not shown ...",
                            g_s.records.size() - shown);
        }
        // The scan's count against this instant's, which is the shrinking made
        // visible. They differed by two in the capture that prompted this, and a
        // dump that printed only one of them would have hidden it.
        Log::get().note(
            "camera view: the scan found %zu record(s); %zu of the %zu shown "
            "still read as records now%s.",
            g_s.records.size(), stillReading, shown,
            stillReading < shown
                ? " -- the array is losing entries while we watch, which is the "
                  "failure itself rather than a bad read"
                : "");
    }

    if (candidates.size() == 1) {
        g_s.chosen = runs[candidates[0]].base + g_s.ordinal * kStride;
        g_s.usable = true;
        g_s.cooldown = 0;
        // The probes above charged the budget while deciding. They were reads of
        // OTHER runs, not of the record finally chosen, so they must not count
        // against it.
        g_s.readFaults = 0;
        g_s.readFaultsNoted = false;
        g_s.badReads = 0;
        Log::get().note("camera view: tracking run %zu, ordinal %zu, which reads "
                        "%u -- a plausible view.",
                        candidates[0], g_s.ordinal, recordValue(g_s.chosen));
        return;
    }
    if (candidates.empty()) {
        // ONE LONG-ENOUGH RUN THAT READ WRONG IS NOT A NEGATIVE YET.
        //
        // The probe above is a single sample taken at whatever instant the walk
        // happened to finish, and that instant is very often a mode change --
        // because what makes a player trigger a scan is walking about. Throwing
        // the run away costs the whole attempt and a minute of cooldown, for a
        // value that is usually correct again within a second.
        //
        // Still exactly one, though. Two long-enough runs is the ambiguity that
        // has to stay a refusal: watching both and taking whichever blinks first
        // is the coin toss with extra steps.
        if (longEnough.size() == 1) {
            const CameraViewRun& r = runs[longEnough[0]];
            g_s.provisional = r.base + g_s.ordinal * kStride;
            g_s.provisionalFrames = kProbeWindow;

            // AN EMPTY SLOT IS NOT A WRONG ANSWER, and must not cost an attempt.
            //
            // Measured: a session where every scan found the array at the address
            // the working sessions used, with 12 of 15 slots holding a record and
            // the ordinal in one of the holes. The game had not finished filling
            // it. Charging that to the four-attempt budget spends the feature's
            // whole allowance waiting for the game to do something it was always
            // going to do -- and the log said "ordinal reads something else",
            // which describes the opposite situation and sent the diagnosis after
            // a wrong anchor.
            if (!slotOccupied(g_s.provisional)) {
                if (g_s.attempts > 0) --g_s.attempts;
                Log::get().note(
                    "camera view: run %zu is the right shape for ordinal %zu, but "
                    "that slot is EMPTY -- %zu of %zu slots hold a record, so the "
                    "array is here and the game has not filled it yet. This does "
                    "not count as an attempt. Watching it for %u frames and "
                    "scanning again after that.",
                    longEnough[0], g_s.ordinal, r.present, r.slots, kProbeWindow);
                return;
            }

            Log::get().note("camera view: run %zu is long enough for ordinal %zu "
                            "but did not read as a view just now. Watching it for "
                            "%u frames before writing it off -- that slot holds "
                            "something else while the game changes mode, and a "
                            "scan finishing at that moment is common.",
                            longEnough[0], g_s.ordinal, kProbeWindow);
            return;
        }
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
    g_s.provisional = nullptr;   // a fresh search supersedes the old maybe
    g_s.provisionalFrames = 0;
    g_s.readFaults = 0;          // a new search gets a fresh budget
    g_s.readFaultsNoted = false;
    g_s.records.clear();
    g_s.regionIndex = 0;
    g_s.regionOffset = 0;
    g_s.bytesScanned = 0;
    g_s.faults = 0;
    collectRegions();
}

void cameraViewTick(uint32_t eyeDraws) {
    // Attempts spent before the game was being played do not count.
    //
    // The scan is triggered by the on-foot panel, which the main menu also
    // satisfies -- so somebody who launches and walks away can exhaust all four
    // attempts on an empty heap and have none left when they start playing. A
    // drawn scene is proof the game is past the menu: thousands of draws into
    // the eye textures, against about twenty for the menu.
    if (!g_s.sawGameplay && eyeDraws > 1000) {
        g_s.sawGameplay = true;
        if (g_s.attempts > 0 && !g_s.usable) {
            Log::get().note("camera view: the game is being played now, so the "
                            "%u attempt(s) made before it was are being refunded "
                            "-- those searched a heap that was not populated yet.",
                            g_s.attempts);
            g_s.attempts = 0;
            g_s.cooldown = 0;
        }
    }
    if (g_s.cooldown > 0) --g_s.cooldown;

    // The run the last scan could not make its mind up about, re-read.
    if (g_s.provisional && !g_s.usable && !g_s.scanning) {
        const uint32_t v = recordValue(g_s.provisional);
        if (v <= g_s.plausibleMax) {
            g_s.chosen = g_s.provisional;
            g_s.usable = true;
            g_s.cooldown = 0;
            g_s.readFaults = 0;
            g_s.readFaultsNoted = false;
            g_s.badReads = 0;
            g_s.provisional = nullptr;
            g_s.provisionalFrames = 0;
            Log::get().note("camera view: that run reads %u now -- a plausible "
                            "view -- so it is being tracked after all. It was "
                            "mid-change when the scan sampled it.", v);
        } else if (--g_s.provisionalFrames == 0) {
            g_s.provisional = nullptr;
            Log::get().note("camera view: that run never read as a view in %u "
                            "frames, so it is not the camera settings after all. "
                            "Waiting for the next scan.", kProbeWindow);
        }
    }

    // A RESCAN ASKS FOR ITSELF. Only the first search waits for the caller.
    //
    // The caller's trigger is the settled flat panel, which is the right moment
    // to search the first time: it is the earliest point the game is known
    // loaded and the player known to be on foot. It is the wrong moment to
    // RE-search, because the array is rebuilt while the player is in the
    // external camera -- where the flat panel is precisely what is not being
    // drawn, and does not come back until they leave. Measured in a user's logs
    // (issue #2): 3m18s and 5m01s between the array going quiet and the rescan
    // that recovered it, both spent waiting for a panel rather than for memory.
    //
    // A RETRY AFTER A FAILED ATTEMPT IS THE SAME PROBLEM. A scan that finishes
    // as the player enters the camera fails (see kProbeWindow) and leaves them
    // in the camera -- so the panel that would ask for the retry does not come
    // back until they give up and leave. Measured: one attempt in a session,
    // the rest of it with no Explorer Cam.
    //
    // The FIRST attempt still waits for the caller. Its timing is the one that
    // genuinely needs judgement -- too early and it searches an empty heap --
    // and `attempts > 0` is proof that judgement has already been exercised
    // once. requestScan does the rest of the gating (cooldown, budgets).
    if (g_s.sawGameplay && !g_s.provisional &&
        (g_s.needRescan || g_s.attempts > 0)) {
        cameraViewRequestScan();
    }

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
    // The recovery branch is keyed on the ANSWER, not on the answer having
    // changed. Edge-gating it on lastView wedged the module silently in one
    // case: a record that dies before its first plausible read leaves lastView
    // at its -1 initialiser, the transition never happens, and no rescan is
    // ever requested. Nothing logs, and the feature is simply off for the
    // session.
    if (out < 0) {
        // HOLD THE ADDRESS FIRST. A bad read is not yet proof of a move.
        //
        // The anchor check proves this address is not a record RIGHT NOW. It
        // does not prove the array went anywhere, and in the field it usually
        // has not: every scan of a reported session re-found the array at the
        // address it had already been at. What actually happens is that the
        // game rebuilds these records when the player's surroundings change,
        // and they read as nothing for a moment while it does.
        //
        // Spending a rescan on that is expensive out of all proportion -- ten
        // gigabytes walked to rediscover the same pointer, and minutes of no
        // Explorer Cam while the trigger comes round again. So: report "do not
        // know" immediately, which takes the offset off and is the safe answer
        // either way, but keep the address and keep reading it.
        if (++g_s.badReads < kMoveGrace) {
            // Once per episode. lastView is cleared just below, so the next
            // frame of the same episode does not come back through here.
            if (g_s.lastView >= 0) {
                Log::get().note("camera view: ordinal %zu stopped reading as a "
                                "camera record. Holding the address for up to %u "
                                "frames before concluding it has moved -- the "
                                "records go quiet for a moment when the game "
                                "rebuilds them, and the offset is off meanwhile.",
                                g_s.ordinal, kMoveGrace);
            }
            g_s.lastView = -1;
            return -1;
        }
        // Not a latch: the array moved, so go and find it again. The anchor
        // check proved this address is no longer a record of ours, which also
        // means a scan can tell the real one from garbage.
        g_s.badReads = 0;
        g_s.usable = false;
        g_s.chosen = nullptr;
        g_s.needRescan = true;
        g_s.cooldown = kRescanCooldown;
        // Reported per MOVE, not once per session. failNoted latched on the
        // first one, so moves 2 through 17 announced nothing at all and the
        // last line anybody saw promised a retry that had already happened
        // fifteen times.
        Log::get().note("camera view: ordinal %zu has not read as a camera record "
                        "for %u frames, so the game really has moved its camera "
                        "settings (move %u). Scanning again to find where they "
                        "went; the offset will not engage until it does. This is "
                        "expected occasionally and is not a fault.",
                        g_s.ordinal, kMoveGrace, g_s.rescans + 1);
        g_s.lastView = -1;
        return -1;
    }
    // A good read ends the episode. Counting a session's total instead would
    // mean a hundred and twenty scattered bad frames across an hour declared a
    // move that never happened.
    if (g_s.badReads > 0) {
        Log::get().note("camera view: ordinal %zu is reading as a camera record "
                        "again after %u frame(s). It had not moved.",
                        g_s.ordinal, g_s.badReads);
        g_s.badReads = 0;
    }
    if (out != g_s.lastView) {
        g_s.lastView = out;
        Log::get().note("camera view is %d (ordinal %zu, read from the game)",
                        out, g_s.ordinal);
    }
    return out;
}

}  // namespace edvr
