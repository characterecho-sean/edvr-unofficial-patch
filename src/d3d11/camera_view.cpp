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
#include "../common/timing.h"
#include "head_offset_gate.h"
#include "journal_watch.h"
#include "vscreen.h"  // kSceneEyeDraws -- the count is vScreen's

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

// The scene-versus-menu draw count -- the fallback for when the journal cannot
// be read -- is vScreen's, because the count itself is. It was a local copy
// here (kMenuEyeDraws) and the comment on it already named the cost of that:
// a third thing to re-measure. See kSceneEyeDraws in vscreen.h for the
// measurement, and cameraViewTick below for what it decides.

// Four attempts, forty seconds apart. Enough to cover a slow load or a player
// who reaches the surface late; few enough that a genuinely wrong anchor -- the
// game-update case -- stops rather than walking the heap forever.
//
// THE CLEAREST CASE IN THE CODEBASE for why durations are not frame counts.
// This was 3600 frames, and the tree documented it three different ways: "a
// minute apart" here, "forty seconds" in camera_view.h, and "about a minute"
// in the line printed to the player. All three were written by someone
// converting 3600 at whatever rate they had in mind -- 60, 90, 60 -- and none
// of them was wrong about the frames or right about the time. Forty seconds is
// the .h's figure, which matches 3600 at the 90Hz the feature was developed on.
constexpr uint32_t kMaxAttempts = 4;
constexpr uint64_t kRetryCooldownMs = 40000;

// Rescans after the array has MOVED, which is a different budget from attempts
// to find it in the first place. Re-finding is cheap to justify -- the array
// demonstrably exists, we had it a moment ago -- so this is generous. It is
// bounded at all only so that a pathological session cannot walk the heap
// forever, and briefly delayed so a move during a mode change is not chased
// several times over.
//
// Sixty-four, up from sixteen, on measurement: near a planet the game
// reshapes the array every ten to thirty seconds -- five moves inside two
// minutes in one capture (6at.3) -- and each reshape is a move, so sixteen
// was a twenty-minute planet session ending with the feature quietly off for
// good. Each rescan walks the heap once (~2 s); sixty-four bounds the
// session's worst case at about two minutes of scanning spread across hours.
constexpr uint32_t kMaxRescans = 64;

// The camera settings records are 0x18 bytes and lie back to back (6ad.7a,
// 6ad.8a: stride 0x18, no gaps, 19 of them). That structure is what tells the
// array apart from unrelated objects of the same type scattered elsewhere.
constexpr size_t kStride = 0x18;
// A few seconds before a rescan is allowed, so a move during a mode change is
// not chased several times over. Was 240 frames: 3.3s at 72Hz, 2s at 120.
constexpr uint64_t kRescanCooldownMs = 2700;

// THE FLAT COOLDOWN IS A FLOOR, AND NEAR A STATION IT BECAME THE RATE.
//
// kMaxRescans is documented above as bounding "the session's worst case at
// about two minutes of scanning spread across hours", and that arithmetic
// assumes rescans are paced by the array actually MOVING -- ten to thirty
// seconds apart. They are not paced by that. A rescan that comes back
// mid-rebuild re-arms on this cooldown alone, and near a station the array is
// mid-rebuild continuously, so the next one fires the moment the cooldown ends.
// Measured in issue #19: 2.7s cooldown plus a 1.5s walk is a rescan every 4.2
// seconds, dead level, for seven minutes -- which spends all sixty-four on one
// station visit and leaves the feature off for the twenty-five minutes of
// session that followed. Not two minutes across hours; two minutes flat, then
// nothing.
//
// So consecutive mid-rebuild answers back off. The FIRST one still retries in
// 2.7 seconds, because a single genuine move is exactly what the rescan budget
// is generous for and nothing about that case has changed. It is a run of them
// -- the signature of a rebuild that is ongoing rather than finished -- that
// doubles the wait. Cumulatively 4.2, 11.1, 23.4, 46.5, 91.2 seconds and so on,
// so a seven-minute station visit is reached around the twelfth or thirteenth
// rescan rather than the sixty-fourth, and the budget survives to serve the
// moves it was sized for.
//
// THAT ARITHMETIC IS THE FLOOR AND NOT THE WHOLE STORY, because this feature's
// own population defeats it: cameraViewNudgeRescan drops the cooldown to 670 ms
// every time the player enters the external camera, deliberately, since cycling
// past fresh candidates is what certifies one. Each nudged cycle still costs its
// cooldown plus a walk, so restoring the old drain would take a camera entry
// every three seconds sustained -- key-mashing, not play. Station photography at
// ten to thirty entries in seven minutes lands two to three times above the
// floor above, still far below sixty-four, and kMaxRescans bounds it either way.
constexpr uint32_t kRebuildBackoffMax = 4;          // 2.7s -> 43.2s
// A guard against a future edit to kRebuildBackoffMax, not the operative bound:
// 2700 << 4 is 43.2s and already under this. Whichever of the two is lower is
// the real ceiling, and it is deliberately the shift, because that is the one
// the comment above does arithmetic with.
constexpr uint64_t kRescanCooldownCapMs = 45000;

// How long a run of mid-rebuild answers stays "the same episode".
//
// EXPIRING ON TIME RATHER THAN RESETTING AT EVERY EXIT, and the reason is that
// the first version of this did the latter and was wrong within a day. A scan
// leaves finishScan by five different paths -- found it, empty slot, one run
// that read wrong, no run fits, mid-rebuild -- and two more sites certify a
// candidate later (pollCandidates, the provisional re-read). Every one of those
// except mid-rebuild ends the episode, so every one of them needed a reset, and
// missing any left the counter standing: a station visit would push the run to
// five, a certification twenty minutes later would leave it there, and the next
// single genuine move -- the ordinary case this whole mechanism promises not to
// touch -- would wait 43 seconds. That is the forty-second dead window the
// rescan budget exists to prevent, reintroduced by the thing meant to protect
// it.
//
// A list of sites to keep in sync is not a rule. Time is: an episode that has
// gone quiet is over, however it ended, and no exit path has to know that. The
// sites that can PROVE the episode ended still say so -- see the resets beside
// each `usable = true` -- because a proof beats an inference and costs a line;
// the timer is what covers the ones nobody remembers.
//
// MEASURED FROM WHEN THE RETRY WAS DUE, NOT FROM WHEN ITS ANSWER ARRIVED, and
// that distinction is the whole correctness of the constant. The gap between
// two mid-rebuild answers is the wait PLUS a heap walk, and the walk's length
// is a user setting: camera_index_mb_per_frame is documented down to 1, and at
// 8 MB/frame a 14 GB walk takes 20 seconds. Judged against answers, the margin
// over the longest backoff (43.2s) was 16.8s, so anyone who lowered that
// setting would have every episode expire, `shift` would never leave 0, and the
// backoff would silently switch itself off -- the exact drain it exists to stop,
// reintroduced by its own guard. Judged against the due time, the walk is out of
// the comparison and this is a grace period for the walk alone.
//
// It is still not infinite: a walk longer than this expires the episode anyway,
// which needs camera_index_mb_per_frame set very low indeed. That failure is
// graceful -- the backoff stands down, kMaxRescans still bounds the session --
// and it is stated rather than claimed away.
constexpr uint64_t kRebuildEpisodeMs = 60000;

// Empty slots the grouping will bridge rather than end a run on. The reasoning,
// and the field evidence for it, is on cameraViewGroupRuns in the header.
//
// Two, not more. One is what has been measured; two gives the same shape a
// little room without letting unrelated objects a few strides apart be swept
// into one array.
constexpr size_t kMaxGap = 2;

// Candidates watched at once, changes needed to be believed, and how often they
// are read.
//
// Three changes because one is noise and two is a coincidence between two
// records; a player cycling presets produces three in a couple of seconds.
// Polled six times a second -- far faster than anybody cycles, and thirty-two
// qword reads at that rate is nothing. Was 15 frames, which is six times a
// second only at 90Hz.
constexpr size_t   kMaxCandidates = 32;
constexpr uint32_t kChangesToCertify = 3;
constexpr uint64_t kCandPollMs = 170;
// How close in TIME a candidate's step must land to a witnessed press to count
// as coincident. Two poll intervals: the write happens within a frame of the
// press and the poll sees it at most one cadence later; this covers that with
// slack while staying far below the seconds between a player's deliberate
// presses. Derived from the poll interval rather than restated, so the two
// cannot drift apart the way the three descriptions of kRetryCooldown did.
constexpr uint64_t kPressCoincidenceMs = 2 * kCandPollMs;
// Places holding the array's address that are worth remembering. Sixty-four is
// far more than a real pointer graph needs and cheap to re-read.
constexpr size_t kMaxAnchorSites = 64;

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
//
// Two seconds, stated as two seconds. As 120 frames it was one second at
// 120Hz -- half the tolerance the sentence above argues for, on the headset
// most likely to be running a busy planet scene in the first place.
constexpr uint64_t kMoveGraceMs = 2000;

// Pointer-hunt bounds. Hunts are re-run when the array is re-found at a NEW
// base while a region list is still in hand; a session that keeps moving is
// capped rather than walked forever. The refind walk tolerates the gaps the
// field dumps show (empty slots mid-array) and stops at more slots than any
// observed layout has had.
constexpr uint32_t kMaxHunts = 6;
constexpr size_t   kMaxRefindSlots = 32;

// Candidate step evidence, capped: the 11:02:28 non-certification could not
// be diagnosed because nothing said whether the watched records stepped at
// all. A handful of lines answers that; a line per step would be the log
// burying itself again.
constexpr uint32_t kStepNotes = 16;

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
// Ten seconds is longer than any mode change observed (25-86 frames, 6ac.6c,
// which is about a second) and short enough that a run which is genuinely not
// the camera settings is not watched for a meaningful part of a session. Was
// 600 frames, which is ten seconds only at 60Hz -- a rate this mod does not
// support -- and 5 seconds at 120Hz.
constexpr uint64_t kProbeWindowMs = 10000;

// The short cooldown a provisional-expiry rescan drops to, overriding whatever
// longer retry cooldown was left over. Was a bare 60 frames.
constexpr uint64_t kShortCooldownMs = 670;

struct Region {
    const uint8_t* base;
    size_t         size;
};

struct State {
    bool      track = false;
    // So the off notice is said once, and again after a hot-reload
    // that switches it back off.
    bool      offNoted = false;
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
    // Deadline rather than countdown: a countdown of frames expires in a
    // different number of seconds on every headset.
    uint64_t  provisionalUntilMs = 0;

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
    // When the cooldown ends. 0 means no cooldown is running.
    uint64_t  cooldownUntilMs = 0;
    bool      exhaustedNoted = false;
    std::vector<const uint8_t*> records;

    // The watched candidates. See the note in finishScan.
    struct Cand {
        const uint8_t* rec;
        CameraViewVote vote;
    };
    std::vector<Cand> cands;
    uint64_t candPollMs = 0;
    // Press-coincidence certification (6aw's consequence): when the next-view
    // key last fired, and whether such a key exists at all. A frame `tick`
    // sat here too, which the coincidence test compared against; that test
    // reads the clock now and nothing else ever read the tick, so it is gone
    // rather than left being incremented for no reader.
    uint64_t lastPressMs = 0;
    bool     pressWitness = false;

    // THE ANCHOR HUNT. Evidence gathering, and deliberately not yet acted on.
    //
    // Near a planet the game rebuilds this array every few seconds -- measured
    // 2026-08-15: a scan succeeded, read a plausible view, and the array had
    // moved three seconds later, twice in one minute. An eleven-gigabyte walk
    // takes about two seconds, so no scan can keep up and no addressing scheme
    // survives, which is why the ordinal, the record-order reading and the
    // behavioural watcher all failed the same way. The premise they shared --
    // that the array is stable and only its layout varies -- is false here.
    //
    // What would survive is a POINTER to the array, because whatever the game
    // updates when it rebuilds is by definition current. So: while an array
    // address is known, remember every place in memory that holds that address.
    // When the array moves, re-read those places. One that now holds the NEW
    // array's address is a live anchor, and re-finding becomes a dereference
    // instead of a heap walk.
    //
    // NOTHING IS ACTED ON UNTIL A CANDIDATE HAS SURVIVED A MOVE. A pointer that
    // merely happens to contain the old address proves nothing -- copies, stale
    // locals and coincidence all look identical until the array moves and only
    // the real one follows. That is the same standard the rest of this file
    // holds: certify by observation, refuse to guess between equals.
    const uint8_t* arrayBase = nullptr;
    std::vector<const uint8_t* const*> anchorSites;
    const uint8_t* huntedBase = nullptr;   // the base the sites were hunted for
    uint32_t hunts = 0;
    uint32_t anchorSurvived = 0;
    uint32_t anchorFollowedMoves = 0;      // moves resolved by a pointer, not a scan
    uint32_t stepNotes = 0;                // candidate step evidence, capped
    bool     candNoted = false;

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
    // Consecutive rescans that came back mid-rebuild, and when the last one
    // landed. The episode ends by going quiet rather than by being reset --
    // see kRebuildEpisodeMs for why that is not the same thing. Drives the
    // backoff described at kRebuildBackoffMax.
    uint32_t  rebuildRuns = 0;
    // When the last mid-rebuild retry was DUE, not when its answer came back.
    // See kRebuildEpisodeMs: the difference is a heap walk whose length is a
    // config value, and comparing against it made the guard setting-dependent.
    uint64_t  rebuildDueMs = 0;
    // Said once, when the rescan budget is gone. Without it the last word in
    // the log is the mid-rebuild line's promise to scan "again in a few
    // seconds", for a scan that is never going to happen -- the same silent
    // give-up the attempts path already learned to announce.
    bool      rescansNoted = false;
    // Whether the scan in flight is a RE-FIND (the array was had a moment ago)
    // rather than a first search. A re-find that comes back unusable retries
    // in seconds on the rescan budget; a first search that fails waits the
    // long cooldown and spends the find budget. See finishScan.
    bool      refinding = false;
    bool      sawGameplay = false;
    // Consecutive frames the chosen record has not read as a record. Reset by
    // any good read, so this counts one episode rather than a session's total.
    uint32_t  badReads = 0;
    // When the current run of bad reads began. The count is kept because it is
    // what the log reports and what the fixtures step; the decision is timed,
    // because "two seconds of holding still" is the thing being argued for.
    uint64_t  badReadsMs = 0;
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

// Collect every place in memory holding 	arget. One pass, capped.
void huntAnchors(const uint8_t* target) {
    g_s.anchorSites.clear();
    if (!target) return;
    for (const Region& r : g_s.regions) {
        if (g_s.anchorSites.size() >= kMaxAnchorSites) break;
        guarded("camera_view/anchorhunt", [&] {
            const uint8_t* const* p =
                reinterpret_cast<const uint8_t* const*>(r.base);
            const size_t n = r.size / sizeof(void*);
            for (size_t i = 0; i < n; ++i) {
                if (p[i] == target) {
                    g_s.anchorSites.push_back(&p[i]);
                    if (g_s.anchorSites.size() >= kMaxAnchorSites) return;
                }
            }
        });
    }
    Log::get().note(
        "camera view: %zu place(s) in memory hold the camera array's address. "
        "When the array moves, whichever of them follows it is a live pointer "
        "to it, and re-finding becomes a dereference instead of an "
        "eleven-gigabyte search -- through the same proving any find passes, "
        "so a pointer that lies costs a probe, never a wrong offset.",
        g_s.anchorSites.size());
}

// After a move: which of them followed?
void checkAnchors(const uint8_t* oldBase) {
    if (g_s.anchorSites.empty()) return;
    size_t followed = 0, stale = 0, gone = 0;
    const uint8_t* firstNew = nullptr;
    for (const uint8_t* const* site : g_s.anchorSites) {
        const uint8_t* now = nullptr;
        const bool ok = guarded("camera_view/anchorcheck",
                                [&] { now = *site; });
        if (!ok) { ++gone; continue; }
        if (now == oldBase) { ++stale; continue; }
        // Does it point at something that looks like one of our records?
        uint32_t v = 0xFFFFFFFFu;
        guarded("camera_view/anchorpeek", [&] {
            const uint8_t* const* anchor =
                reinterpret_cast<const uint8_t* const*>(now);
            if (*anchor == g_s.typePtr) v = 0;
        });
        if (v == 0) { ++followed; if (!firstNew) firstNew = now; }
    }
    ++g_s.anchorSurvived;
    Log::get().note(
        "camera view: the array moved, and of the %zu place(s) that held its "
        "address, %zu now point at a camera record, %zu still hold the old "
        "address and %zu could not be read. %s",
        g_s.anchorSites.size(), followed, stale, gone,
        followed == 1
            ? "Exactly one followed, which is what a live pointer to the array "
              "looks like."
            : "Not yet conclusive.");
}

// ONE PLACE THAT LEARNS THE ARRAY'S ADDRESS, called from every path that finds
// it. There are three -- the scan's single-candidate branch, the behavioural
// watcher, and the provisional run proving itself over 600 frames -- and the
// hunt was wired to the first only. A whole test flight produced no anchor
// evidence because of it: the array was found by another path, so nothing was
// ever recorded to compare against when it moved.
void noteArrayBase(const uint8_t* base) {
    if (!base) {
        // Reachable from exactly one caller: behavioural certification while a
        // rescan is in flight, before its first match -- the record list is
        // empty, so there is no address to hunt from. Said out loud rather
        // than returned from silently: a silent branch in the instrument that
        // exists to explain silences is how 6ao's archaeology restarts.
        Log::get().note(
            "camera view: the array's base is not known at this instant (the "
            "record list is mid-rescan), so no pointer hunt from this find.");
        return;
    }
    if (g_s.arrayBase && base != g_s.arrayBase) checkAnchors(g_s.arrayBase);
    g_s.arrayBase = base;
    // Re-hunt for every NEW base while a region list is in hand -- the
    // one-shot hunt left later homes with no pointers at all, so the first
    // move was the last one a pointer could ever resolve.
    if (base == g_s.huntedBase) return;
    if (g_s.hunts >= kMaxHunts) return;
    if (g_s.regions.empty()) {
        // Said out loud ONCE rather than logging "0 places found", which
        // reads as evidence when it is the absence of a search. Later finds
        // reach here routinely (a pointer-followed base has no fresh region
        // list), and that is not news.
        if (g_s.hunts == 0) {
            Log::get().note(
                "camera view: cannot look for pointers to the camera array -- "
                "the region list was released when the scan finished, so there "
                "is nothing to search. No anchor evidence from this find.");
        }
        return;
    }
    ++g_s.hunts;
    g_s.huntedBase = base;
    huntAnchors(base);
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

// Rebuild the candidate watch list from g_s.records, CARRYING earned votes
// for records still at the same address. Rescans run every few seconds
// during churn -- and one now fires on every camera entry -- so the old
// unconditional wipe restarted every candidate's two-step walk mid-stride,
// which is one suspect for the 11:02:28 non-certification the step notes
// exist to settle.
void rebuildCands() {
    std::vector<State::Cand> prior;
    prior.swap(g_s.cands);
    size_t carried = 0;
    for (const uint8_t* rec : g_s.records) {
        const uint32_t v = recordValue(rec);
        if (v > g_s.plausibleMax) continue;
        if (g_s.cands.size() >= kMaxCandidates) break;
        State::Cand c{};
        c.rec = rec;
        const State::Cand* old = nullptr;
        for (const State::Cand& p : prior) {
            if (p.rec == rec) { old = &p; break; }
        }
        if (old) {
            c.vote = old->vote;   // progress survives the refresh
            ++carried;
        } else {
            c.vote.last = v;
            c.vote.primed = true;
            // Anchored when the record already reads the view the gate's
            // count predicts (0 after a disembark, the last confirmed view
            // within a session): such a candidate certifies on the two steps
            // the player's own walk to their preset supplies.
            c.vote.anchored =
                v == static_cast<uint32_t>(headOffsetGateCountedView());
        }
        g_s.cands.push_back(c);
    }
    if (!g_s.cands.empty()) {
        char carriedNote[64] = "";
        if (carried) {
            snprintf(carriedNote, sizeof(carriedNote),
                     " (%zu carried forward with their progress)", carried);
        }
        Log::get().note(
            "camera view: watching %zu candidate record(s)%s to see which one "
            "behaves like a camera preset -- a small number that stays in range "
            "and CHANGES when you cycle cameras. Cycle through your presets once "
            "and it will identify itself. Until then the preset is unknown and "
            "the head offset stays off.",
            g_s.cands.size(), carriedNote);
    }
}

// At read-death, before spending a scan: do the remembered pointer sites
// nominate a new home? A nomination is NOT a certification -- whatever they
// point at re-enters through the same proving every find passes (the
// ordinal through its 600-frame probe, the watcher through behavioural
// certification), so a wrong pointer costs a probe, never a wrong offset.
// Refusals mirror the scan's: sites that disagree are none of them believed.
bool anchorRefind() {
    if (g_s.anchorSites.empty()) return false;
    const uint8_t* nominated = nullptr;
    bool disagree = false;
    for (const uint8_t* const* site : g_s.anchorSites) {
        const uint8_t* now = nullptr;
        if (!guarded("camera_view/anchorfollow", [&] { now = *site; })) continue;
        if (!now || now == g_s.arrayBase) continue;   // stale: the dead home
        if (!slotOccupied(now)) continue;             // target is not a record
        if (!nominated) { nominated = now; continue; }
        if (now != nominated) disagree = true;
    }
    if (!nominated) return false;
    if (disagree) {
        Log::get().note(
            "camera view: the remembered pointers disagree about where the "
            "array went, so none of them is believed and the scan decides.");
        return false;
    }
    // The census line the anchor hunt promised: which sites followed.
    checkAnchors(g_s.arrayBase);
    // Walk the nominated base the way the scan reports one: records at the
    // known stride, gaps tolerated, capped above any observed layout.
    g_s.records.clear();
    for (size_t slot = 0; slot < kMaxRefindSlots; ++slot) {
        const uint8_t* rec = nominated + slot * kStride;
        if (slotOccupied(rec)) g_s.records.push_back(rec);
    }
    if (g_s.records.size() < 3) {
        g_s.records.clear();
        return false;
    }
    ++g_s.anchorFollowedMoves;
    rebuildCands();
    // The ordinal re-enters as PROVISIONAL, and only when the walk is
    // gapless through it -- the provisional prover back-computes the base
    // by ordinal * stride, which a gap would silently shift.
    const uint8_t* ordRec = g_s.records.size() > g_s.ordinal
                                ? g_s.records[g_s.ordinal] : nullptr;
    const bool gapless =
        ordRec == nominated + g_s.ordinal * kStride;
    if (ordRec && gapless) {
        g_s.provisional = ordRec;
        g_s.provisionalUntilMs = nowMs() + kProbeWindowMs;
    }
    g_s.arrayBase = nominated;
    Log::get().note(
        "camera view: a remembered pointer already names the array's new home "
        "-- %zu record(s) at %p, adopted for proving instead of walking the "
        "heap (pointer follow %u). %s",
        g_s.records.size(), (const void*)nominated, g_s.anchorFollowedMoves,
        (ordRec && gapless)
            ? "The ordinal re-enters through its probe and the watcher gets "
              "the records as candidates."
            : "The ordinal slot is empty or behind a gap, so the watcher "
              "alone decides from these candidates.");
    return true;
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

    // THE SCAN FINISHED SECOND; THE QUESTION WAS ALREADY ANSWERED.
    //
    // The behavioural watcher keeps polling the old candidates while a rescan
    // walks the heap -- deliberately, because the watcher answering mid-scan
    // is the fast path when the game rebuilds the array. Measured 2026-08-15
    // (6ar): the watcher certified the preset's new slot one second into an
    // eleven-gigabyte walk, and 1.5 seconds later this function's
    // unconditional `chosen = nullptr` threw that answer away. `usable` stayed
    // true, so the read path refused on !chosen, the poll and the scan
    // request both gated off on usable, and the module was silently wedged
    // for the rest of the session -- the exact shape this file has hunted
    // twice before. A scan that completes after certification is stale
    // evidence about an answered question: say so and change nothing,
    // including the attempt budget, which is for FINDING.
    if (g_s.usable && g_s.chosen) {
        Log::get().note(
            "camera view: the scan finished after the preset had already been "
            "identified by behaviour, so its result is not needed and nothing "
            "changes. (The identification stands; this line exists because a "
            "scan completion used to overwrite it.)");
        return;
    }

    ++g_s.attempts;
    g_s.cooldownUntilMs = nowMs() + kRetryCooldownMs;
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

    // BEHAVIOURAL IDENTIFICATION. Every plausible record becomes a candidate,
    // and the one that BEHAVES like a camera preset is the one taken.
    //
    // A static ordinal cannot be right. Four layouts of the same array have been
    // recorded inside one build -- 10, 12, 14 and 19 records, with the preset at
    // slot 11 in the twelve-record form and garbage there in the nineteen. Both
    // readings of "ordinal 11", counted over records and over slots, point at
    // the same wrong place in the 19-record capture, so neither is the rule.
    //
    // What a preset does is unmistakable: it holds a small integer, it stays in
    // range, and it CHANGES when the player cycles cameras. Nothing else in that
    // array does all three. So the candidates are watched instead of chosen, and
    // this certifies like every other invariant here -- on observation, with
    // "do not know" as the answer until the evidence arrives, and with a refusal
    // rather than a guess when more than one candidate qualifies.
    //
    // The ordinal survives as a HINT and only as a tie-break, which is what it
    // has always deserved: it was measured twice and then contradicted.
    rebuildCands();

    if (candidates.size() == 1) {
        g_s.chosen = runs[candidates[0]].base + g_s.ordinal * kStride;
        // The array's own base, which is what a pointer to it would hold.
        noteArrayBase(runs[candidates[0]].base);
        g_s.usable = true;
        g_s.cooldownUntilMs = 0;
        // Finding it PROVES the rebuild episode ended; the timer only infers
        // it, and only after a minute. Without this line a station episode that
        // ends in a successful find leaves the run standing, and a single
        // genuine move thirty seconds later waits 43 seconds -- the timer does
        // not save it, because thirty seconds is not a minute. A proof is worth
        // the line wherever one exists; kRebuildEpisodeMs covers the rest.
        g_s.rebuildRuns = 0;
        // The probes above charged the budget while deciding. They were reads of
        // OTHER runs, not of the record finally chosen, so they must not count
        // against it.
        g_s.readFaults = 0;
        g_s.readFaultsNoted = false;
        g_s.badReads = 0;
        g_s.badReadsMs = 0;
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
            g_s.provisionalUntilMs = nowMs() + kProbeWindowMs;

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
                    "not count as an attempt. Watching it for %u seconds and "
                    "scanning again after that.",
                    longEnough[0], g_s.ordinal, r.present, r.slots,
                    (unsigned)(kProbeWindowMs / 1000));
                return;
            }

            Log::get().note("camera view: run %zu is long enough for ordinal %zu "
                            "but did not read as a view just now. Watching it for "
                            "%u seconds before writing it off -- that slot holds "
                            "something else while the game changes mode, and a "
                            "scan finishing at that moment is common.",
                            longEnough[0], g_s.ordinal,
                            (unsigned)(kProbeWindowMs / 1000));
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

    // A RE-FIND THAT CAME BACK UNUSABLE IS NOT A FAILED SEARCH. The records
    // are right there, at the base they have always been at -- the layout is
    // just mid-rebuild, and near a planet it rebuilds every ten to thirty
    // seconds (6at.3). Charging the find budget and waiting the long cooldown
    // turned each rebuild into a forty-second dead window, stacked (6at.2).
    // So: give back the attempt, and go again in a few seconds on the rescan
    // budget, which is sized for exactly this churn. The behavioural watcher
    // keeps the candidate list this scan just built in the meantime -- fresh
    // candidates are the thing the player's cycling needs to meet.
    if (g_s.refinding) {
        if (g_s.attempts > 0) --g_s.attempts;
        g_s.needRescan = true;
        // The whole decision is cameraViewRebuildBackoff, which is pure and
        // has a table in gate_test. Nothing about it is judged here.
        const CameraViewBackoff bo =
            cameraViewRebuildBackoff(g_s.rebuildRuns, g_s.rebuildDueMs, nowMs());
        const uint64_t wait = bo.waitMs;
        g_s.rebuildRuns = bo.runs;
        g_s.cooldownUntilMs = bo.dueMs;
        // The episode's own clock. Deliberately NOT cooldownUntilMs, which
        // cameraViewNudgeRescan lowers to 670 ms on a camera entry -- reading
        // that back would shorten the grace by however much the player nudged.
        g_s.rebuildDueMs = bo.dueMs;
        // "NOT BEFORE", because this is a floor and not a schedule.
        // cameraViewNudgeRescan lowers the cooldown to 670 ms when the player
        // enters the external camera, and its guards are all false in exactly
        // this state -- so a player who cycles cameras brings the next scan
        // forward, and a line promising 43200 ms would be describing something
        // that did not happen. Which is the failure the give-up line below was
        // just added to fix; it is not worth reintroducing two hunks earlier.
        Log::get().note(
            "camera view: the array is here (%zu record(s) at its usual base) "
            "but mid-rebuild. Not scanning again for %llu ms -- unless you "
            "enter the external camera, which brings the next one forward to "
            "about 0.7 s -- rather than spending a search attempt on a layout "
            "that will not be the layout by then. Mid-rebuild answer %u in a "
            "row, and the wait doubles "
            "with them: a rebuild that is still going is not worth re-walking "
            "the heap for at full rate. %u of %u rescans left.",
            g_s.records.size(), (unsigned long long)wait, g_s.rebuildRuns,
            kMaxRescans > g_s.rescans ? kMaxRescans - g_s.rescans : 0,
            kMaxRescans);
        return;
    }
    Log::get().note("camera view:%s", retry);
}

}  // namespace

CameraViewBackoff cameraViewRebuildBackoff(uint32_t runs, uint64_t dueMs,
                                           uint64_t now) {
    // An episode that has gone quiet is over, whichever path ended it --
    // measured against when the retry was DUE, so that a slow heap walk is
    // not mistaken for quiet. See kRebuildEpisodeMs.
    if (dueMs != 0 && now > dueMs + kRebuildEpisodeMs) runs = 0;
    // Doubling on the RUN, not on the total: the first mid-rebuild answer of
    // an episode waits the flat cooldown, as it always did.
    const uint32_t shift = runs < kRebuildBackoffMax ? runs : kRebuildBackoffMax;
    uint64_t wait = kRescanCooldownMs << shift;
    if (wait > kRescanCooldownCapMs) wait = kRescanCooldownCapMs;
    return {runs + 1, wait, now + wait};
}

void cameraViewConfigure() {
    Config& cfg = Config::get();
    const bool wasTracking = g_s.track;
    g_s.track = cfg.getBool("d3d11.camera_index_track", false);
    // OFF is the shipped state now, and silence would read as a fault. The
    // one thing this module must never do is stop without saying so: every
    // other way it can fail already announces itself, and a default that
    // went quiet would be the exception that sends someone hunting.
    if (!g_s.track) {
        if (!g_s.offNoted || wasTracking) {
            g_s.offNoted = true;
            Log::get().note(
                "camera view: off (d3d11.camera_index_track = 0), which is the "
                "shipped default. Which camera preset you are on is counted from "
                "your keypresses instead of read from the game -- anchored at "
                "launch and at every new on-foot session, so it is right unless a "
                "press goes unseen, and leaving the camera and re-entering resets "
                "it. Turning this on searches eleven to seventeen gigabytes for "
                "the records and needs a marker measured on YOUR game build.");
        }
        return;
    }
    g_s.offNoted = false;
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
    if (g_s.cooldownUntilMs != 0 && nowMs() < g_s.cooldownUntilMs) return;
    // A rescan after a move does not spend the find-it-first-time budget.
    if (g_s.needRescan) {
        if (g_s.rescans >= kMaxRescans) {
            // Once, so the log distinguishes "gave up" from "still waiting".
            // The line before this one promised another scan in a few seconds,
            // and without this that promise is the last thing anybody reads.
            if (!g_s.rescansNoted) {
                g_s.rescansNoted = true;
                Log::get().note(
                    "camera view: %u rescans used and the array was never caught "
                    "in a settled state, so which camera preset you are on is "
                    "unknown for the rest of this session. No further scan will "
                    "run -- the earlier line promising one in a few seconds is "
                    "superseded by this. Cycling cameras will not recover it; a "
                    "relaunch will.",
                    g_s.rescans);
            }
            return;
        }
        ++g_s.rescans;
        g_s.needRescan = false;
        g_s.refinding = true;
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
    } else {
        g_s.refinding = false;   // an ordinary attempt spends the find budget
    }
    g_s.scanning = true;
    g_s.chosen = nullptr;
    g_s.provisional = nullptr;   // a fresh search supersedes the old maybe
    g_s.provisionalUntilMs = 0;
    g_s.readFaults = 0;          // a new search gets a fresh budget
    g_s.readFaultsNoted = false;
    g_s.records.clear();
    g_s.regionIndex = 0;
    g_s.regionOffset = 0;
    g_s.bytesScanned = 0;
    g_s.faults = 0;
    collectRegions();
}

// Read a CANDIDATE, without touching the chosen record's fault budget.
//
// recordValue exists to read ONE known address every frame, and its budget of
// eight is sized for that: a handful of failures there means the address is
// gone. Polling ten candidates through it spends that budget in seconds, and
// once spent recordValue answers 0xFFFFFFFF for everything -- which the poll
// below then read as "out of range" and used to disqualify every candidate
// permanently. Measured: "9 faults reading the chosen record", after which the
// watcher could never identify anything, so cycling presets did nothing.
//
// Speculative reads of addresses we are not yet sure about are a different
// activity from reading the one we chose, and they get their own accounting.
uint32_t candidateValue(const uint8_t* rec) {
    if (!rec) return 0xFFFFFFFFu;
    uint32_t v = 0xFFFFFFFFu;
    guarded("camera_view/cand", [&] {
        const uint8_t* const* anchor = reinterpret_cast<const uint8_t* const*>(rec);
        if (*anchor != g_s.typePtr) return;
        v = *reinterpret_cast<const uint32_t*>(rec + g_s.valueOffset);
    });
    return v;
}
// The one judgement both field-caught certification bugs lived in, pure so
// the frame-feed test can replay them on a desk. The rules, each bought by a
// flight:
//
// STEP (6au): a change counts only when the value steps UP BY EXACTLY ONE --
// rebuild noise writes arbitrary values, presses step the cycle. Any other
// change resets everything: ignored instead, a slot oscillating 0-1-0-1
// banks a +1 at every rise. The 5-to-0 wrap resets too; a forward loop still
// supplies five sequential steps.
//
// WITNESS (6aw): the array contains a counter that rebuilds increment
// sequentially -- indistinguishable from presses by shape -- so an in-camera
// requirement joins the step (the preset can only change while the player is
// in the camera), and with a next-view key bound, certification needs TWO
// steps each landing beside a witnessed press. A counter ticking between
// presses accrues plain changes, which the witnessed bar ignores; the rare
// storm where its ticks straddle the player's presses lands in the
// two-qualifiers refusal rather than in a wrong certification.
//
// Without a bound key there are no witnessed presses, so the legacy bar --
// three sequential in-camera steps -- still stands for that configuration,
// with 6aw's in-camera-counter exposure documented as its cost.
bool cameraViewCertStep(CameraViewVote* c, uint32_t v, bool inCamera,
                        bool pressRecent, bool witnessed) {
    if (!c->primed) {
        c->primed = true;
        c->last = v;
        return false;
    }
    if (v != c->last) {
        if (v == c->last + 1 && inCamera) {
            ++c->changes;
            if (pressRecent) ++c->coincident;
        } else {
            c->changes = 0;
            c->coincident = 0;
            // A broken sequence forfeits the anchor too: the anchor's claim
            // was about stepping FROM the predicted value, and this stopped.
            c->anchored = false;
        }
        c->last = v;
    }
    if (witnessed) return c->coincident >= 2;
    // Anchored two-step: primed at the value the gate predicted and stepped
    // twice in-camera since -- the player's own walk from the opening view
    // to their preset. The unanchored bar stays at three.
    if (c->anchored && c->changes >= 2) return true;
    return c->changes >= kChangesToCertify;
}

void cameraViewNotePress() { g_s.lastPressMs = stampMs(); }

void cameraViewSetPressWitness(bool nextKeyBound) {
    g_s.pressWitness = nextKeyBound;
}

void cameraViewNudgeRescan() {
    // A camera entry with nothing certified: the player is about to cycle,
    // and cycling past fresh candidates is what certifies. Same shape as the
    // provisional-expiry rescan (6as): the rescan budget, a short cooldown
    // override, and the tick self-trigger picks it up.
    if (g_s.usable || g_s.scanning || !g_s.sawGameplay) return;
    g_s.needRescan = true;
    const uint64_t shortUntil = nowMs() + kShortCooldownMs;
    if (g_s.cooldownUntilMs > shortUntil) g_s.cooldownUntilMs = shortUntil;
}

// Watch the candidates for one that behaves like a camera preset.
//
// Certifies on the SAME terms as everything else in this codebase: observation,
// a refusal to guess between equals, and "do not know" as a legitimate answer.
// If two records both qualify we have learned that we cannot tell them apart,
// which is information -- guessing between them would put a wrong number into
// the head-offset gate and move somebody's viewpoint on the strength of it.
void pollCandidates() {
    if (g_s.usable || g_s.cands.empty()) return;
    if (!dueMs(g_s.candPollMs, kCandPollMs)) return;
    g_s.candPollMs = stampMs();

    const bool inCamera = headOffsetGateInCamera();
    const bool pressRecent =
        g_s.lastPressMs != 0 &&
        (nowMs() - g_s.lastPressMs) <= kPressCoincidenceMs;

    size_t qualified = 0, qualifiedAt = 0;
    for (size_t i = 0; i < g_s.cands.size(); ++i) {
        State::Cand& c = g_s.cands[i];
        if (!c.rec) continue;                       // already disqualified
        const uint32_t v = candidateValue(c.rec);
        // UNREADABLE IS NOT OUT OF RANGE, and conflating them is what broke
        // this. 0xFFFFFFFF means the anchor did not match or the read faulted --
        // the records go briefly quiet while the game rebuilds them, which is
        // routine -- so the sample is skipped rather than held against it.
        if (v == 0xFFFFFFFFu) continue;
        // OUT OF RANGE DEMOTES, IT NO LONGER EXECUTES (6at.4: the record that
        // IS the preset read 8 during a rebuild transition). The change count
        // resets on every out-of-range sample, so a wandering slot still can
        // never accumulate; the genuine preset merely restarts its progress.
        if (v > g_s.plausibleMax) {
            c.vote.changes = 0;
            c.vote.coincident = 0;
            continue;
        }
        // The certification decision itself lives in cameraViewCertStep --
        // pure, and driven directly by the frame-feed test, because both of
        // this module's field-caught certification bugs (6au, 6aw) were in
        // exactly this judgement and cost a flight each to find.
        const uint32_t beforeChanges = c.vote.changes;
        const uint32_t beforeLast = c.vote.last;
        const bool certified = cameraViewCertStep(&c.vote, v, inCamera,
                                                  pressRecent, g_s.pressWitness);
        // Step evidence, capped: the 11:02:28 non-certification was
        // undiagnosable because nothing said whether the watched records
        // stepped at all during the player's cycling.
        if (c.vote.changes > beforeChanges && g_s.stepNotes < kStepNotes) {
            ++g_s.stepNotes;
            Log::get().note(
                "camera view: watched record %zu stepped %u -> %u in the "
                "camera (%u sequential%s%s). Capped at %u such lines a "
                "session.",
                i, beforeLast, v, c.vote.changes,
                c.vote.anchored ? ", anchored at the predicted start" : "",
                c.vote.coincident ? ", press-coincident" : "",
                kStepNotes);
        } else if (beforeChanges > 0 && c.vote.changes == 0 &&
                   g_s.stepNotes < kStepNotes) {
            ++g_s.stepNotes;
            Log::get().note(
                "camera view: watched record %zu broke its sequence "
                "(%u -> %u %s), so its %u step(s) start over.",
                i, beforeLast, v,
                inCamera ? "in the camera" : "OUT of the camera",
                beforeChanges);
        }
        if (certified) {
            ++qualified;
            qualifiedAt = i;
        }
    }

    if (qualified == 1) {
        g_s.chosen = g_s.cands[qualifiedAt].rec;
        // Lowest-addressed record found, which is the array's base for a
        // contiguous run. A base that is slightly off finds nothing and says
        // so; not looking at all finds nothing and says nothing.
        noteArrayBase(g_s.records.empty() ? nullptr : g_s.records.front());
        g_s.usable = true;
        g_s.rebuildRuns = 0;   // certified: the rebuild episode is provably over
        g_s.readFaults = 0;
        g_s.readFaultsNoted = false;
        const State::Cand& won = g_s.cands[qualifiedAt];
        Log::get().note(
            "camera view: identified by behaviour -- the record at %p stepped "
            "in range %u time(s)%s, and nothing else in the array did. That is "
            "your camera preset, and it reads %u now. Found by watching rather "
            "than by counting to a fixed position, because four different "
            "array layouts have been seen in one game build.",
            (const void*)g_s.chosen, won.vote.changes,
            g_s.pressWitness
                ? " with your view-key press landing beside each counted step"
                : (won.vote.anchored
                       ? " from exactly the view the count predicted"
                       : " while you cycled"),
            recordValue(g_s.chosen));
        return;
    }

    if (qualified > 1 && !g_s.candNoted) {
        g_s.candNoted = true;
        Log::get().note(
            "camera view: %zu records changed like a camera preset, so which one "
            "it is cannot be told from here and the preset stays unknown. This is "
            "a refusal rather than a failure -- guessing would move your viewpoint "
            "on the strength of the wrong number. Please report this log.",
            qualified);
    }
}
void cameraViewTick(uint32_t eyeDraws) {
    // Attempts spent before the game was being played do not count.
    //
    // The scan is triggered by the on-foot panel, which the main menu also
    // satisfies -- so somebody who launches and walks away can exhaust all four
    // attempts on an empty heap and have none left when they start playing.
    //
    // THE JOURNAL STATES THIS, and the draw count infers it. 6ba is the
    // standing rule that where the journal speaks, the heuristic becomes the
    // fallback for when it does not (disabled, folder not found, fault budget
    // spent).
    //
    // BUT THE FALLBACK WINS IN PRACTICE, and saying otherwise here would be a
    // comment describing a design rather than the code. Measured on both
    // headsets 2026-08-17: the draw count latched 45 s before LoadGame on the
    // Quest 3 and 25 s before it on the Pimax, because Elite renders a full
    // scene while loading in and only writes LoadGame at the end of it. With
    // an OR, the earlier signal always decides, so the journal is a backstop
    // for a starved recogniser rather than the authority.
    //
    // That is accepted rather than fixed. What latching does is REFUND scan
    // attempts, so being early costs at most the one or two attempts made in
    // that window -- against a failure mode, never latching at all, that
    // costs the feature for the session. Vetoing on an active-but-silent
    // journal would trade a bounded cost for an unbounded one, which is the
    // wrong direction for a signal whose job is to be permissive.
    //
    // THE FALLBACK THRESHOLD WAS WRONG, AND MEASURABLY SO. It read
    // `eyeDraws > 1000` on the premise of "thousands of draws into the eye
    // textures, against about twenty for the menu". The twenty is still right.
    // The thousands were an artefact of counting targets that were not eye
    // textures: once the submitted size is narrowed by its bounds (6bl), a
    // whole session peaks at 975 on a Quest 3 and 1074 on a Pimax -- so 1000
    // sits INSIDE the range it was meant to be far above, and on the Quest 3
    // sawGameplay never latched at all. The refund and the rescan self-trigger
    // were both dead there, silently.
    //
    // 100 is the measurement, not a margin picked to be safe. Menu-only
    // sessions peak at 20 and 22 (2026-08-14 15:48, 2026-08-15 16:21, neither
    // reaching LoadGame), and the flash detector's own validation measured the
    // same 0-to-22 over 300 menu frames. Gameplay clears 100 even in sessions
    // quit seconds after loading in (peaks of 119 and 126). It is the number
    // glitch_frame already uses for "this frame rendered a scene", which is
    // the same physical question; a third number for it would be a third
    // thing to re-measure.
    //
    // Either signal may latch. The consequence of latching is a REFUND, and
    // the failure being fixed is never latching at all, so the two sources are
    // ORed rather than the journal being allowed to veto: a journal that is
    // off, lagging its one-second poll, or that missed the event still leaves
    // a working path, and each source is independently sound.
    pollCandidates();

    const bool journalSaysPlaying = journalWatchActive() && journalGameplay();
    if (!g_s.sawGameplay && (journalSaysPlaying || eyeDraws > kSceneEyeDraws)) {
        g_s.sawGameplay = true;
        // Which source said so, because they can disagree and the difference
        // is diagnosable: the journal naming it means the boundary is exact,
        // the draw count naming it means the journal was not available and a
        // proxy was used. A log that does not distinguish them cannot answer
        // "was the journal being read?" after the fact.
        // THREE CASES, NOT TWO. The first cut printed "no journal" whenever
        // the draw count won, which was a plain misstatement: on both field
        // runs the journal was being read perfectly well and simply had not
        // reached LoadGame yet. A log line that names the wrong cause sends
        // the next reader to check a folder that was never the problem.
        Log::get().note("camera view: the game is being played now (%s). "
                        "Attempts made before this point searched a heap that "
                        "was not populated yet.",
                        journalSaysPlaying
                            ? "the journal's LoadGame"
                            : journalWatchActive()
                                  ? "a drawn scene, ahead of the journal -- "
                                    "the game renders while it loads in and "
                                    "writes LoadGame at the end of that"
                                  : "no journal is being read, so a drawn "
                                    "scene stood in for it");
        if (g_s.attempts > 0 && !g_s.usable) {
            Log::get().note("camera view: %u attempt(s) refunded.", g_s.attempts);
            g_s.attempts = 0;
            g_s.cooldownUntilMs = 0;
        }
    }
    // No per-frame decrement: a deadline does not need one, and the old
    // countdown was the thing that made the cooldown headset-dependent.

    // The run the last scan could not make its mind up about, re-read.
    if (g_s.provisional && !g_s.usable && !g_s.scanning) {
        const uint32_t v = recordValue(g_s.provisional);
        if (v <= g_s.plausibleMax) {
            g_s.chosen = g_s.provisional;
            noteArrayBase(g_s.provisional - g_s.ordinal * kStride);
            g_s.usable = true;
            g_s.cooldownUntilMs = 0;
            g_s.rebuildRuns = 0;   // the provisional held: episode provably over
            g_s.readFaults = 0;
            g_s.readFaultsNoted = false;
            g_s.badReads = 0;
            g_s.badReadsMs = 0;
            g_s.provisional = nullptr;
            g_s.provisionalUntilMs = 0;
            Log::get().note("camera view: that run reads %u now -- a plausible "
                            "view -- so it is being tracked after all. It was "
                            "mid-change when the scan sampled it.", v);
        } else if (g_s.provisionalUntilMs != 0 &&
                   nowMs() >= g_s.provisionalUntilMs) {
            g_s.provisionalUntilMs = 0;
            g_s.provisional = nullptr;
            // ARM THE RESCAN THIS MESSAGE USED TO ONLY PROMISE. The empty-slot
            // branch refunds its attempt, and the refund zeroes the very
            // condition (`attempts > 0`) the self-trigger fires on -- so the
            // expiry left the module waiting for the settled-flat-panel
            // caller, which cannot fire while the player is in the camera
            // (6ae.7, re-entered through the refund; measured as a 72-second
            // dead window in 6as, spent exactly while the player was there
            // cycling). A re-find on the rescan budget, with the short
            // cooldown overriding finishScan's leftover retry cooldown: the
            // array demonstrably exists, and the watcher needs candidates
            // FRESHER than the last rebuild -- the tail relocates repeatedly,
            // so only a prompt scan can hand it addresses worth watching.
            g_s.needRescan = true;
            g_s.cooldownUntilMs = nowMs() + kRescanCooldownMs;
            Log::get().note("camera view: that run never read as a view in %u "
                            "seconds, so it is not the camera settings after "
                            "all. Scanning again in a few seconds.",
                            (unsigned)(kProbeWindowMs / 1000));
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
    if (!g_s.track) return -1;
    // `usable && !chosen` is a state no path should produce -- usable means
    // "a record was chosen and certified" -- and it is also a state that used
    // to be PERMANENT: the read path refused on !chosen while the poll and
    // the scan request both gated off on usable, so nothing could ever mend
    // it and nothing ever logged (6ar: 3m45s of silence after a completing
    // scan clobbered a mid-scan certification). The finishScan guard removes
    // the known producer; this converts any future producer into one log
    // line and a rescan instead of a feature that is quietly off until
    // relaunch.
    if (g_s.usable && !g_s.chosen) {
        Log::get().note(
            "camera view: certified but nothing chosen -- a state that should "
            "be impossible and used to wedge this feature for the session. "
            "Recovering with a rescan; please report this log.");
        g_s.usable = false;
        g_s.needRescan = true;
        g_s.cooldownUntilMs = nowMs() + kRescanCooldownMs;
    }
    if (!g_s.usable || !g_s.chosen) return -1;
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
        ++g_s.badReads;
        if (g_s.badReads == 1) g_s.badReadsMs = stampMs();
        if (!elapsedMs(g_s.badReadsMs, kMoveGraceMs)) {
            // Once per episode. lastView is cleared just below, so the next
            // frame of the same episode does not come back through here.
            if (g_s.lastView >= 0) {
                Log::get().note("camera view: ordinal %zu stopped reading as a "
                                "camera record. Holding the address for up to %u "
                                "seconds before concluding it has moved -- the "
                                "records go quiet for a moment when the game "
                                "rebuilds them, and the offset is off meanwhile.",
                                g_s.ordinal,
                                (unsigned)(kMoveGraceMs / 1000));
            }
            g_s.lastView = -1;
            return -1;
        }
        // Not a latch: the array moved, so go and find it again. The anchor
        // check proved this address is no longer a record of ours, which also
        // means a scan can tell the real one from garbage.
        // Read out BEFORE the reset below, because the line at the end of
        // this branch reports them. Clearing first made it print "for 0
        // frames" on every move -- the counter it was added to carry.
        const uint32_t badFrames = g_s.badReads;
        const uint64_t badMs = g_s.badReadsMs ? nowMs() - g_s.badReadsMs : 0;
        g_s.badReads = 0;
        g_s.badReadsMs = 0;
        g_s.usable = false;
        g_s.chosen = nullptr;
        // A remembered pointer may already name the new home: one guarded
        // dereference against ten gigabytes walked, and the nomination still
        // has to prove itself the way any find does.
        if (anchorRefind()) {
            g_s.lastView = -1;
            return -1;
        }
        g_s.needRescan = true;
        g_s.cooldownUntilMs = nowMs() + kRescanCooldownMs;
        // Reported per MOVE, not once per session. failNoted latched on the
        // first one, so moves 2 through 17 announced nothing at all and the
        // last line anybody saw promised a retry that had already happened
        // fifteen times.
        Log::get().note("camera view: ordinal %zu has not read as a camera record "
                        "for %u frames over %llu ms, so the game really has "
                        "moved its camera settings (move %u). Scanning again to "
                        "find where they went; the offset will not engage until "
                        "it does. This is expected occasionally and is not a "
                        "fault.",
                        g_s.ordinal, badFrames,
                        (unsigned long long)badMs, g_s.rescans + 1);
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
        g_s.badReadsMs = 0;
    }
    if (out != g_s.lastView) {
        g_s.lastView = out;
        Log::get().note("camera view is %d (ordinal %zu, read from the game)",
                        out, g_s.ordinal);
    }
    return out;
}

}  // namespace edvr
