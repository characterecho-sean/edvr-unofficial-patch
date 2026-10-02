#include "journal_watch.h"

#include "../common/timing.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/periodic_work.h"

namespace edvr {
namespace {

// Every half second. Halved from once a second on 2026-08-16: the keyless
// entry latch waits on this cadence -- measured sample arrivals landed +53 and
// +90 frames after a panel stop -- and a 2 KB Status read plus a bounded
// journal slice at this rate costs nothing measurable.
//
// Was 45 frames, which is half a second at 90Hz and 0.63/0.38 at the other two.
constexpr uint64_t kPollMs = 500;

// Re-glob for a newer journal file every eighth poll: a new part or a relaunch
// appears within that, and directory listings are the expensive half.
//
// The comment here used to say "~12 s", which was true when the poll was 90
// frames and somebody assumed 60Hz. Nobody updated it when the poll was halved,
// so it was wrong by 3x in a file whose whole subject is cadence. Derived from
// the poll interval now, so it cannot drift again.
constexpr uint32_t kReglobPolls = 8;
constexpr uint64_t kReglobMs = kReglobPolls * kPollMs;   // 4 s

// Status.json's own clock when a consumer asked for low latency (the FSS
// mode latch -- a screen that engages a second late is a screen the player
// watched arrive). Otherwise it rides the journal's kPollMs.
constexpr uint64_t kStatusEagerMs = 100;

// Consecutive file-op failures before the watcher retires for the session.
constexpr uint32_t kMaxFaults = 8;

// The largest slice read per poll. Journal bursts (Materials, Statistics at
// login) run tens of kilobytes; one slice a poll keeps up with anything the
// game writes and bounds what a single poll reads either way.
constexpr uint32_t kReadChunk = 64 * 1024;

// The rig's scale on the three cadences above (journal_watch.h). 100 is the
// shipped cadence and the only value the product ever runs at.
std::atomic<uint32_t> g_cadencePct{100};

uint64_t cadence(uint64_t ms) {
    const uint64_t scaled = ms * g_cadencePct.load(std::memory_order_relaxed) / 100;
    return scaled ? scaled : 1;
}

// THE THREAD BOUNDARY, in three pieces.
//
//   Published  what the worker has learned, as one plain value it builds up as
//              it reads and hands over whole.
//   Visible    the copy every accessor answers from. Written by the tick on the
//              Present thread, and only from a whole Published (configure
//              resets it before any tick), so the answers change together at
//              the frame boundary -- a consumer that reads onFootKnown, onFoot
//              and the sample count in one frame cannot get one from each side
//              of a Status read. Atomic fields, relaxed loads: readers on other
//              threads see each field whole, and on x64 that is the plain load
//              these were before.
//   Session    the worker's own state, the mailbox and the stop signal, one per
//              worker, never freed (see journalWatchShutdown).

// Everything a consumer can ask this module, as one value.
struct Published {
    // False once the fault budget is spent (or the worker cannot run at all):
    // the callers' cue to fall back to heuristics.
    bool     active = true;
    bool     gameplay = false;
    uint32_t disembarks = 0;
    uint32_t embarks = 0;
    // When a jump tunnel MAY be on screen: armed at StartJump, cleared by
    // the event that resolves it (FSDJump for hyperspace, SupercruiseEntry
    // for the other branch -- StartJump fires for both, and telling them
    // apart by NAME keeps the names-only posture). A cancelled charge
    // resolves with neither, which is what the expiry in the accessor is
    // for. Zero means not armed.
    uint64_t jumpArmedMs = 0;
    // Status Flags bit 30, the FSD jump itself. StartJump fires at the
    // COUNTDOWN, five seconds before any tunnel exists, and the field found
    // the difference immediately: the forming-wormhole sprite is the same
    // family, world-anchored, and was being pinned on the pad. The flag is
    // what narrows the window to the tunnel; `known` is false when Status
    // lacks the field, and the accessor then falls back to a countdown-
    // length delay after StartJump.
    bool     fsdJumpKnown = false;
    bool     fsdJumpLive = false;
    // Live Flags2 from Status.json: OnFoot is bit 0, InTaxi is bit 1.
    // `onFootKnown` is false whenever the file lacks a Flags2 field, which
    // is exactly the menu and shutdown states -- Status then carries only
    // "Flags":0.
    bool     onFootKnown = false;
    bool     onFoot = false;
    bool     inTaxiKnown = false;
    bool     inTaxi = false;
    // Status Flags bit 24 (InMainShip), bit 25 (InFighter -- SLFs and SLVs
    // including the Nomad), bit 26 (InSRV).
    bool     inMainShipKnown = false;
    bool     inMainShip = false;
    bool     inFighterKnown = false;
    bool     inFighter = false;
    bool     inSrvKnown = false;
    bool     inSrv = false;
    // GuiFocus from Status.json: 9 is the Full System Scanner. The game
    // states the MODE outright -- entry and exit by any path (keybind,
    // ESC, an interdiction yanking the player out of supercruise) all
    // land here, which no keypress watcher can promise. Supercruise is
    // Flags bit 4, read to qualify FSS-key presses (the key does nothing
    // outside supercruise).
    bool     fssFocusKnown = false;   // GuiFocus is in Status.json
    bool     fssFocus = false;
    uint32_t guiFocus = 0;            // its value, while known
    bool     supercruiseKnown = false;
    bool     supercruise = false;
    uint32_t statusSamples = 0;       // successful Status.json parses
};

struct Visible {
    std::atomic<bool>     active{false};
    std::atomic<bool>     gameplay{false};
    std::atomic<uint32_t> disembarks{0};
    std::atomic<uint32_t> embarks{0};
    std::atomic<uint64_t> jumpArmedMs{0};
    std::atomic<bool>     fsdJumpKnown{false};
    std::atomic<bool>     fsdJumpLive{false};
    std::atomic<bool>     onFootKnown{false};
    std::atomic<bool>     onFoot{false};
    std::atomic<bool>     inTaxiKnown{false};
    std::atomic<bool>     inTaxi{false};
    std::atomic<bool>     inMainShipKnown{false};
    std::atomic<bool>     inMainShip{false};
    std::atomic<bool>     inFighterKnown{false};
    std::atomic<bool>     inFighter{false};
    std::atomic<bool>     inSrvKnown{false};
    std::atomic<bool>     inSrv{false};
    std::atomic<bool>     fssFocusKnown{false};
    std::atomic<bool>     fssFocus{false};
    std::atomic<uint32_t> guiFocus{0};
    std::atomic<bool>     supercruiseKnown{false};
    std::atomic<bool>     supercruise{false};
    std::atomic<uint32_t> statusSamples{0};
};
Visible g_v;

template <class T>
T peek(const std::atomic<T>& a) {
    return a.load(std::memory_order_relaxed);
}

void applyPublished(const Published& p) {
    constexpr std::memory_order kRelaxed = std::memory_order_relaxed;
    g_v.gameplay.store(p.gameplay, kRelaxed);
    g_v.disembarks.store(p.disembarks, kRelaxed);
    g_v.embarks.store(p.embarks, kRelaxed);
    g_v.jumpArmedMs.store(p.jumpArmedMs, kRelaxed);
    g_v.fsdJumpKnown.store(p.fsdJumpKnown, kRelaxed);
    g_v.fsdJumpLive.store(p.fsdJumpLive, kRelaxed);
    g_v.onFootKnown.store(p.onFootKnown, kRelaxed);
    g_v.onFoot.store(p.onFoot, kRelaxed);
    g_v.inTaxiKnown.store(p.inTaxiKnown, kRelaxed);
    g_v.inTaxi.store(p.inTaxi, kRelaxed);
    g_v.inMainShipKnown.store(p.inMainShipKnown, kRelaxed);
    g_v.inMainShip.store(p.inMainShip, kRelaxed);
    g_v.inFighterKnown.store(p.inFighterKnown, kRelaxed);
    g_v.inFighter.store(p.inFighter, kRelaxed);
    g_v.inSrvKnown.store(p.inSrvKnown, kRelaxed);
    g_v.inSrv.store(p.inSrv, kRelaxed);
    g_v.fssFocusKnown.store(p.fssFocusKnown, kRelaxed);
    g_v.fssFocus.store(p.fssFocus, kRelaxed);
    g_v.guiFocus.store(p.guiFocus, kRelaxed);
    g_v.supercruiseKnown.store(p.supercruiseKnown, kRelaxed);
    g_v.supercruise.store(p.supercruise, kRelaxed);
    g_v.statusSamples.store(p.statusSamples, kRelaxed);
    g_v.active.store(p.active, std::memory_order_release);
}

// A new watcher starts from nothing. The game configures once and this is a
// no-op on a fresh process; a rig configures several times in one.
void resetVisible() {
    Published nothing;
    nothing.active = false;
    applyPublished(nothing);
}

// Set by the consumers from any thread (journalWatchSetEagerStatus), read by the
// worker on every pass. Not part of a session: the game sets it right after
// configuring, and it means the same thing to whichever worker is running.
std::atomic<bool> g_eager{false};

// The rig's hook (journal_watch.h). Null in the product.
std::atomic<JournalWorkHook> g_workHook{nullptr};

void ioSite(const char* site) {
    if (const JournalWorkHook hook = g_workHook.load(std::memory_order_relaxed)) hook(site);
}

struct Session {
    // ---- Fixed before the thread starts; read-only after.
    std::wstring dir;
    // Files older than EDVR's own start are the PREVIOUS session's journal:
    // replaying one would fire last session's boundaries into this one.
    FILETIME     notBefore = {};

    // ---- The worker's own. Nothing on this side of the line is touched by any
    // other thread while the worker runs.
    Published    pub;            // built up as it reads, handed over by publish()
    uint64_t     statusMs = 0;   // Status.json's own clock
    uint64_t     pollMs = 0;     // last poll
    uint64_t     reglobMs = 0;   // last directory re-glob
    uint32_t     faults = 0;
    bool         faultsNoted = false;
    std::wstring file;           // the journal currently tailed
    HANDLE       handle = INVALID_HANDLE_VALUE;
    uint64_t     offset = 0;
    // Said once: a journal was adopted that we could not prove is ours, so it
    // is being tailed from the end rather than replayed. See newestJournal.
    // `ownNoted` retires that statement when a proven-ours file replaces it.
    bool         foreignNoted = false;
    bool         ownNoted = false;
    bool         sizeFailNoted = false;
    // Token carry across read chunks, so an event name split by a chunk
    // boundary is still seen. Sixteen bytes covers `"event":"` plus slack.
    char         carry[32] = {};
    uint32_t     carryLen = 0;
    // Consecutive Status.json parse misses. The game rewrites the file about
    // once a second and a read can land mid-write; one blip must not read as
    // the player teleporting off their feet, so `known` only drops after a
    // few misses in a row.
    uint32_t     statusMisses = 0;
    // Phase-0 timing for the three things this module does with files
    // (src/common/periodic_work.h says what is written and why). Each is timed
    // separately because they run on different clocks and cost different
    // things: the Status.json read as often as every 100 ms, the tail slice
    // every 500 ms, and the directory walk every four seconds, whose price
    // grows with every journal the player has ever kept. PeriodicWork belongs to
    // one thread, and these belong to the worker: they were the Present thread's
    // until the file work moved.
    PeriodicWork workStatus{"journal_status"};
    PeriodicWork workTail{"journal_tail", "bytes"};
    PeriodicWork workReglob{"journal_reglob", "files"};
    // SEH containment, as the frame boundary's tick had: a fault in a pass is
    // absorbed and logged once, and a run of them retires the watcher.
    FaultBudget  budget{"journal.worker", 8};

    // ---- The mailbox. The worker waits on the mutex; the Present thread only
    // ever try-locks it, and holds it for one struct copy.
    std::mutex            mailMutex;
    Published             mail;
    std::atomic<uint32_t> mailSeq{0};   // bumped under the mutex on each publish

    // ---- The Present thread's.
    uint32_t              applied = 0;  // the mailSeq last taken
    std::atomic<bool>     started{false};

    // ---- Control.
    HANDLE                wake = nullptr;   // auto-reset: stop, and an eager change
    std::atomic<bool>     stop{false};
    std::atomic<bool>     exited{false};
};

// The current session. Sessions are never freed: the worker may be finishing a
// file call when shutdown returns, and the menu's writer and the panel raster
// worker are leaked for the same reason (see log.cpp, "deliberately leaked").
// One per configure, so a worker that is still winding down when the next
// configure comes cannot touch what the new one is using. That keeps the DATA
// alive; the worker's CODE stays mapped by the CRT's thread-start reference and
// by the graphics DLL's pin (module_pin.h): see startWorker.
std::atomic<Session*> g_session{nullptr};

// Set by shutdown. A frame that arrives after it starts nothing.
std::atomic<bool> g_stopped{false};

void closeFile(Session& s) {
    if (s.handle != INVALID_HANDLE_VALUE) {
        CloseHandle(s.handle);
        s.handle = INVALID_HANDLE_VALUE;
    }
    s.offset = 0;
    s.carryLen = 0;
}

// FILETIME is a 64-bit count in two halves; the decision below wants one
// number, and a fixture wants one it can write as a literal.
uint64_t asU64(const FILETIME& t) {
    return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

// The newest Journal.*.log that could belong to THIS session, or empty.
// `ours` reports whether the winner was CREATED since we started.
//
// This is the directory walk only. Which candidate wins, and why that test is
// creation time rather than write time, is journalPickNewest in the header --
// where the reasoning sits beside the decision it argues for, and where a
// fixture can reach it.
//
// `enumerated` is how many journal files the walk found, for the phase-0
// timing: the walk's cost is the number of files in the folder.
std::wstring newestJournal(const Session& s, bool& ours, size_t& enumerated) {
    enumerated = 0;
    ioSite("reglob");
    // The walk collects; journalPickNewest decides. The decision is a pure
    // function over times so it can be put in a table -- see its header.
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = s.dir + L"\\Journal.*.log";
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return std::wstring();
    std::vector<std::wstring> names;
    std::vector<uint64_t> creation, write;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        names.push_back(fd.cFileName);
        creation.push_back(asU64(fd.ftCreationTime));
        write.push_back(asU64(fd.ftLastWriteTime));
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    enumerated = names.size();

    const JournalPick pick = journalPickNewest(
        creation.data(), write.data(), names.size(), asU64(s.notBefore));
    ours = pick.ours;
    return pick.index < 0 ? std::wstring()
                          : names[static_cast<size_t>(pick.index)];
}

void onEvent(Session& s, const char* name, uint32_t len) {
    // The five that matter. Everything else in the stream is somebody else's
    // business, and not keeping it is the privacy posture: names only, and
    // only these names do anything.
    if (len == 8 && memcmp(name, "LoadGame", 8) == 0) {
        if (!s.pub.gameplay) {
            s.pub.gameplay = true;
            Log::get().note("journal: LoadGame -- gameplay has started, so the "
                            "camera hotkeys mean the camera now rather than a "
                            "menu.");
        }
    } else if (len == 9 && memcmp(name, "Disembark", 9) == 0) {
        ++s.pub.disembarks;
    } else if (len == 6 && memcmp(name, "Embark", 6) == 0) {
        ++s.pub.embarks;
    } else if (len == 8 && memcmp(name, "Shutdown", 8) == 0) {
        // The game is leaving. Gameplay ends with it; a relaunch gets a new
        // journal and a fresh LoadGame.
        s.pub.gameplay = false;
        s.pub.jumpArmedMs = 0;
    } else if (len == 9 && memcmp(name, "StartJump", 9) == 0) {
        s.pub.jumpArmedMs = stampMs();
    } else if (len == 7 && memcmp(name, "FSDJump", 7) == 0) {
        // Hyperspace arrival: the tunnel is over.
        s.pub.jumpArmedMs = 0;
    } else if (len == 16 && memcmp(name, "SupercruiseEntry", 16) == 0) {
        // StartJump's other branch: this was a supercruise transition, not a
        // hyperspace tunnel.
        s.pub.jumpArmedMs = 0;
    }
    // Embark / Touchdown / Liftoff are recognised by name here should a
    // consumer ever need them; today the ones above carry the features.
}

const char kEventTok[] = "\"event\":\"";
constexpr uint32_t kEventTokLen = sizeof(kEventTok) - 1;

void scanRange(Session& s, const char* p, uint32_t n) {
    for (uint32_t i = 0; i + kEventTokLen < n; ++i) {
        if (memcmp(p + i, kEventTok, kEventTokLen) != 0) continue;
        const uint32_t start = i + kEventTokLen;
        uint32_t end = start;
        while (end < n && p[end] != '"' && end - start < 40) ++end;
        if (end < n && p[end] == '"') onEvent(s, p + start, end - start);
        i = end;
    }
}

// Status.json, reread whole on the same cadence: it is a few hundred bytes,
// rewritten by the game about once a second. Two facts are taken from it:
// Flags2's OnFoot bit, and Flags' FSD-jump bit (30). A file without the
// field -- the menu, shutdown -- answers "not known", and callers fall back
// to keys and delays respectively.
void pollStatus(Session& s) {
    ioSite("status");
    const std::wstring path = s.dir + L"\\Status.json";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    bool parsed = false;
    bool sawFlags2 = false;
    bool sawFlags = false;
    bool sawGui = false;
    uint32_t flags2 = 0;
    uint32_t flags = 0;
    uint32_t gui = 0;
    if (f != INVALID_HANDLE_VALUE) {
        char buf[2048];
        DWORD got = 0;
        if (ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr) && got > 0) {
            buf[got] = '\0';
            parsed = true;
            const char* p = strstr(buf, "\"Flags2\":");
            if (p) {
                sawFlags2 = true;
                flags2 = static_cast<uint32_t>(strtoul(p + 9, nullptr, 10));
            }
            // "Flags2" contains "Flags" as a substring, so the plain field
            // is found by requiring the quote to close right after it.
            const char* q = strstr(buf, "\"Flags\":");
            if (q) {
                sawFlags = true;
                flags = static_cast<uint32_t>(strtoul(q + 8, nullptr, 10));
            }
            const char* g = strstr(buf, "\"GuiFocus\":");
            if (g) {
                sawGui = true;
                gui = static_cast<uint32_t>(strtoul(g + 11, nullptr, 10));
            }
        }
        CloseHandle(f);
    }
    if (parsed) {
        s.statusMisses = 0;
        ++s.pub.statusSamples;
        s.pub.onFootKnown = sawFlags2;
        s.pub.onFoot = sawFlags2 && (flags2 & 0x01u) != 0;
        s.pub.inTaxiKnown = sawFlags2;
        s.pub.inTaxi = sawFlags2 && (flags2 & 0x02u) != 0;
        // Bit 30 of Flags: the FSD jump itself -- the tunnel, not the
        // countdown before it. The distinction is what scopes the witchspace
        // star fix off the forming-wormhole phase, where the game still
        // positions the same sprite family correctly in the world.
        s.pub.fsdJumpKnown = sawFlags;
        s.pub.fsdJumpLive = sawFlags && (flags & 0x40000000u) != 0;
        s.pub.supercruiseKnown = sawFlags;
        s.pub.supercruise = sawFlags && (flags & 0x10u) != 0;
        // Vehicle flags: bit 24 InMainShip, bit 25 InFighter (SLF/Nomad), bit 26 InSRV
        s.pub.inMainShipKnown = sawFlags;
        s.pub.inMainShip = sawFlags && (flags & 0x01000000u) != 0;
        s.pub.inFighterKnown = sawFlags;
        s.pub.inFighter = sawFlags && (flags & 0x02000000u) != 0;
        s.pub.inSrvKnown = sawFlags;
        s.pub.inSrv = sawFlags && (flags & 0x04000000u) != 0;
        const bool fss = sawGui && gui == 9;
        if (fss != s.pub.fssFocus) {
            Log::get().note(fss ? "status: GuiFocus 9 -- the game says the "
                                  "player is in the Full System Scanner."
                                : "status: the game says FSS focus ended.");
        }
        s.pub.fssFocusKnown = sawGui;
        s.pub.fssFocus = fss;
        s.pub.guiFocus = sawGui ? gui : 0;
    } else if (++s.statusMisses >= 3) {
        s.pub.onFootKnown = false;
        s.pub.inTaxiKnown = false;
        s.pub.inTaxi = false;
        s.pub.fsdJumpKnown = false;
        s.pub.fssFocusKnown = false;
        s.pub.fssFocus = false;
        s.pub.supercruiseKnown = false;
        s.pub.inMainShipKnown = false;
        s.pub.inMainShip = false;
        s.pub.inFighterKnown = false;
        s.pub.inFighter = false;
        s.pub.inSrvKnown = false;
        s.pub.inSrv = false;
    }
}

// Scan a buffer for `"event":"NAME"`, with a small carry so a token split
// across chunk boundaries is still found.
void scanEvents(Session& s, const char* data, uint32_t len) {
    // Stitch the carry to the front so a split token reassembles.
    char stitched[sizeof(s.carry) + 256];
    uint32_t stitchedLen = 0;
    if (s.carryLen) {
        memcpy(stitched, s.carry, s.carryLen);
        const uint32_t take = len < 256 ? len : 256;
        memcpy(stitched + s.carryLen, data, take);
        stitchedLen = s.carryLen + take;
    }
    if (stitchedLen) scanRange(s, stitched, stitchedLen);
    scanRange(s, data, len);

    // Keep the tail as the next carry. Anything already scanned in `stitched`
    // that also sits in `data`'s head double-scans harmlessly: LoadGame and
    // Shutdown are idempotent and a Disembark token cannot span BOTH the old
    // carry and the new tail.
    const uint32_t keep =
        len < sizeof(s.carry) ? len : static_cast<uint32_t>(sizeof(s.carry));
    memcpy(s.carry, data + (len - keep), keep);
    s.carryLen = keep;
}

// Everything that is due, once. This is what journalWatchTick() used to do on
// the Present thread; it runs on the worker now and is otherwise the same code.
// Returns false when the watcher has retired.
bool pass(Session& s, char* buf) {
    // dueMs, not elapsedMs: the counter this replaced started at 0 meaning
    // "poll on the first frame", and a stamp of 0 read as "never due" would
    // have retired the watcher before it ever ran -- silently, because the
    // only write to the stamp is the line below, inside the branch it gates.
    // Status.json rides its own clock: ~1 KB reread, normally on the
    // journal's cadence, but at 100 ms when a consumer asked for low
    // latency.
    const bool eager = g_eager.load(std::memory_order_relaxed);
    if (dueMs(s.statusMs, cadence(eager ? kStatusEagerMs : kPollMs))) {
        s.statusMs = stampMs();
        PeriodicWorkScope timing(s.workStatus);
        pollStatus(s);
    }

    if (!dueMs(s.pollMs, cadence(kPollMs))) return true;
    s.pollMs = stampMs();

    // Find or refresh the file being tailed.
    if (s.handle == INVALID_HANDLE_VALUE || dueMs(s.reglobMs, cadence(kReglobMs))) {
        s.reglobMs = stampMs();
        bool ours = false;
        std::wstring newest;
        {
            // The walk and the pick only, not the file open below: that runs
            // when the journal CHANGES, which is once in a while, not on the
            // four-second beat this is timing.
            PeriodicWorkScope timing(s.workReglob);
            size_t enumerated = 0;
            newest = newestJournal(s, ours, enumerated);
            timing.setContext(enumerated);
        }
        if (!newest.empty() && newest != s.file) {
            closeFile(s);
            s.file = newest;
            ioSite("open");
            s.handle = CreateFileW((s.dir + L"\\" + s.file).c_str(),
                                   GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE |
                                       FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (s.handle == INVALID_HANDLE_VALUE) {
                if (++s.faults > kMaxFaults) goto retire;
                s.file.clear();
                return true;
            }
            // A file we can prove is ours replays from the top: its early lines
            // are this session's LoadGame, which is exactly the state being
            // rebuilt. One we cannot is tailed from the END instead -- live
            // events from it are still worth having, and are still ours if the
            // game is in fact writing them, but its HISTORY belongs to whoever
            // wrote it and must not fire boundaries into this session.
            if (!ours) {
                LARGE_INTEGER size{};
                if (!GetFileSizeEx(s.handle, &size) || size.QuadPart < 0) {
                    // WITHOUT THE SIZE THERE IS NO TAIL, only a replay -- and a
                    // replay of an unproven file is the whole bug. Leaving
                    // s.offset at the 0 that closeFile set would revert to it
                    // silently, which is the failure mode this module keeps
                    // meeting.
                    //
                    // CHARGED AND SAID ONCE, both of which the first version of
                    // this branch skipped and both of which it needed. Dropping
                    // the handle re-arms the reglob immediately -- the gate above
                    // short-circuits on INVALID_HANDLE_VALUE and does not wait
                    // the four seconds -- so a persistent failure comes back
                    // every poll, twice a second. Unlatched, that is 7000 log
                    // lines an hour and the 4 MB cap inside three hours (the default
                    // is 16 MB since 2026-10-01: twelve), which is
                    // the instrument destroying the evidence for the third time
                    // in this codebase. Uncharged, the watcher never retires
                    // either, because kMaxFaults is what retires it.
                    const DWORD err = GetLastError();
                    closeFile(s);
                    s.file.clear();
                    if (!s.sizeFailNoted) {
                        s.sizeFailNoted = true;
                        Log::get().note(
                            "journal: could not measure a journal that cannot be "
                            "proved to be this session's (error %lu), so it is "
                            "being left alone rather than replayed from the top. "
                            "Retrying quietly; if this is the only journal there "
                            "is, gameplay boundaries will come from the render "
                            "state instead.",
                            err);
                    }
                    if (++s.faults > kMaxFaults) goto retire;
                    return true;
                }
                s.offset = static_cast<uint64_t>(size.QuadPart);
                if (!s.foreignNoted) {
                    s.foreignNoted = true;
                    Log::get().note(
                        "journal: %S was written since this process started but "
                        "created before it, so it cannot be proved to be this "
                        "session's. Reading it from the end rather than replaying "
                        "it -- the usual cause is a previous run that crashed and "
                        "was relaunched within the half-minute, whose LoadGame "
                        "would otherwise be read as this one's.",
                        s.file.c_str());
                }
            } else if (s.foreignNoted && !s.ownNoted) {
                // The line above stands as the last word on provenance
                // otherwise, and a support reader would take it for the whole
                // session. Say when it stops being true: the game's own journal
                // usually appears within a reglob or two of the foreign one.
                s.ownNoted = true;
                Log::get().note(
                    "journal: %S was created after this process started, so it "
                    "IS this session's and is being read in full. That "
                    "supersedes the line above about reading from the end.",
                    s.file.c_str());
            }
        }
        if (s.handle == INVALID_HANDLE_VALUE) return true;   // nothing yet
    }

    // Read whatever has appeared since last time.
    {
        // Timed to the end of the block whichever way it is left: the failed
        // reads that return or retire below are runs too. The bytes read are
        // its context, which is what tells a slow slice from a large one.
        PeriodicWorkScope timing(s.workTail);
        ioSite("tail");
        LARGE_INTEGER pos;
        pos.QuadPart = static_cast<LONGLONG>(s.offset);
        if (!SetFilePointerEx(s.handle, pos, nullptr, FILE_BEGIN)) {
            if (++s.faults > kMaxFaults) goto retire;
            return true;
        }
        DWORD got = 0;
        if (!ReadFile(s.handle, buf, kReadChunk, &got, nullptr)) {
            if (++s.faults > kMaxFaults) goto retire;
            return true;
        }
        timing.setContext(got);
        if (got > 0) {
            s.offset += got;
            scanEvents(s, buf, got);
            s.faults = 0;
        }
    }
    return true;

retire:
    if (!s.faultsNoted) {
        s.faultsNoted = true;
        Log::get().note("journal: %u file errors in a row, so the journal is "
                        "not being read for the rest of this session. The "
                        "render-state heuristics carry everything, as before.",
                        s.faults);
    }
    closeFile(s);
    s.pub.active = false;
    return false;
}

// The pass, inside the project's fault containment. A fault is absorbed and
// logged once at this site, as it was inside the frame boundary's budget; when
// the budget is spent the watcher retires the way a run of file errors does.
bool guardedPass(Session& s, char* buf) {
    bool alive = true;
    if (!guardedBudget(s.budget, [&] { alive = pass(s, buf); })) {
        if (!s.budget.shouldRun()) {
            if (!s.faultsNoted) {
                s.faultsNoted = true;
                Log::get().note("journal: the reader faulted repeatedly, so the "
                                "journal is not being read for the rest of this "
                                "session. The render-state heuristics carry "
                                "everything, as before.");
            }
            closeFile(s);
            s.pub.active = false;
            return false;
        }
    }
    return alive;
}

void publish(Session& s) {
    std::lock_guard<std::mutex> lock(s.mailMutex);
    s.mail = s.pub;
    s.mailSeq.store(s.mailSeq.load(std::memory_order_relaxed) + 1,
                    std::memory_order_release);
}

// How long until the next thing is due, for the wait between passes. A stamp of
// 0 is a cadence that has never run and is due now (timing.h, dueMs).
DWORD untilNextDue(const Session& s) {
    const uint64_t now = nowMs();
    auto left = [now](uint64_t since, uint64_t every) -> uint64_t {
        if (since == 0) return 0;
        const uint64_t age = now - since;
        return age >= every ? 0 : every - age;
    };
    const bool eager = g_eager.load(std::memory_order_relaxed);
    const uint64_t status = left(s.statusMs, cadence(eager ? kStatusEagerMs : kPollMs));
    const uint64_t poll = left(s.pollMs, cadence(kPollMs));
    uint64_t ms = status < poll ? status : poll;
    // Never a spin: a pass that is still due after running (a slow disk made it
    // longer than its own cadence) gets a millisecond, not none.
    if (ms < 1) ms = 1;
    return static_cast<DWORD>(ms > 0x7FFFFFFFull ? 0x7FFFFFFFull : ms);
}

// The worker. Runs a pass, hands over what it has, waits for the next thing to
// be due or for a stop or an eager change to wake it, and repeats.
void workerBody(Session& s) {
    SetThreadDescription(GetCurrentThread(), L"edvr-journal");
    Log::get().note("journal: reading on its own thread (%lu), off the render "
                    "thread.", GetCurrentThreadId());
    // 64 KB, and this thread's: held for the worker's life rather than for the
    // DLL's as the function-local static it replaces was.
    std::unique_ptr<char[]> buf(new (std::nothrow) char[kReadChunk]);
    if (!buf) {
        Log::get().note("journal: no memory for the read buffer, so the "
                        "journal is not being read. The render-state heuristics "
                        "carry everything, as before.");
        s.pub.active = false;
        publish(s);
        return;
    }
    while (!s.stop.load(std::memory_order_acquire)) {
        const bool alive = guardedPass(s, buf.get());
        publish(s);
        if (!alive) break;
        WaitForSingleObject(s.wake, untilNextDue(s));
    }
    // Its own handle, closed by its own thread: nothing else touches it.
    closeFile(s);
}

void workerMain(Session* sp) {
    Session& s = *sp;
    // No exception leaves this thread: an uncaught one is std::terminate, which
    // is the game.
    try {
        workerBody(s);
    } catch (...) {
        // Whatever it was, this worker is finished. Tell the consumers, rather
        // than leave them the last values for good.
        closeFile(s);
        s.pub.active = false;
        try {
            publish(s);
        } catch (...) {
        }
    }
    s.exited.store(true, std::memory_order_release);
}

// A detached thread that runs this module's code for the whole session, so the
// module must not be unmapped under it. Two things see to that, and the code is
// safe on either alone. std::thread starts it through the static UCRT's
// _beginthreadex, which takes a reference on the module of the thread routine and
// ends the thread through FreeLibraryAndExitThread (ucrt\startup\thread.cpp): a
// FreeLibrary during a file call leaves the image mapped. And the graphics DLL is
// pinned (module_pin.h) at the first device creation, before the first Present
// tick gets here, which does not depend on the CRT. gate_test's exe cannot be
// unloaded at all. tools/journal_unload_test loads this code in a DLL and shows
// both across a FreeLibrary, and shows a CreateThread thread and a pool callback
// lose the image without the pin. The RC4 review (F1) supposed neither held.
bool startWorker(Session& s) {
    try {
        std::thread(workerMain, &s).detach();
        return true;
    } catch (...) {
        return false;
    }
}

// Stop whatever worker is current and forget it. Used when a new watcher is
// configured; shutdown does the stopping half itself.
void endSession() {
    if (Session* old = g_session.exchange(nullptr)) {
        old->stop.store(true, std::memory_order_release);
        SetEvent(old->wake);
    }
}

}  // namespace

JournalPick journalPickNewest(const uint64_t* creation, const uint64_t* write,
                              size_t count, uint64_t notBefore) {
    JournalPick best;
    uint64_t bestWrite = 0;
    for (size_t i = 0; i < count; ++i) {
        // Tier one: is it live at all? A file untouched since we started
        // cannot be the journal of a session that is running now.
        if (write[i] < notBefore) continue;
        const bool born = creation[i] >= notBefore;
        // Tier two: provenance outranks recency. Until the game creates its
        // own journal the only candidate may be a foreign one, and the moment
        // ours appears it must win -- even for the seconds before the game has
        // written enough to it to be the most recently touched file there.
        //
        // bestWrite going BACKWARDS when a born file displaces a non-born one
        // is correct, not a bug: the third clause only ever compares within one
        // provenance class, so a value carried over from the other class is
        // never consulted against it.
        const bool better = best.index < 0 || (born && !best.ours) ||
                            (born == best.ours && write[i] > bestWrite);
        if (better) {
            best.index = static_cast<int>(i);
            best.ours = born;
            bestWrite = write[i];
        }
    }
    return best;
}

void journalWatchConfigure() {
    // A new watcher starts from nothing, and stops the one before it, if any.
    endSession();
    g_stopped.store(false);
    resetVisible();

    Config& cfg = Config::get();
    if (!cfg.getBool("d3d11.journal_watch", true)) return;

    FILETIME notBefore = {};
    GetSystemTimeAsFileTime(&notBefore);
    // Half a minute of slack: the journal for THIS process is created around
    // the moment this code runs, and file times are not promised to be
    // ordered against ours to the millisecond.
    //
    // newestJournal applies this to CREATION time to decide whether a journal
    // is ours, and to write time only to decide whether it is live at all. The
    // slack is sized for the first question. Read the second comment there
    // before widening it: thirty seconds against write time is what let a
    // crashed session's journal be replayed into the next one (issue #19).
    ULARGE_INTEGER t;
    t.LowPart = notBefore.dwLowDateTime;
    t.HighPart = notBefore.dwHighDateTime;
    t.QuadPart -= 30ull * 10000000ull;
    notBefore.dwLowDateTime = t.LowPart;
    notBefore.dwHighDateTime = t.HighPart;

    std::wstring dir;
    const std::string dirUtf8 = cfg.getString("d3d11.journal_dir", "");
    if (!dirUtf8.empty()) {
        const int need = MultiByteToWideChar(CP_UTF8, 0, dirUtf8.c_str(), -1,
                                             nullptr, 0);
        std::wstring w(need > 0 ? need : 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, dirUtf8.c_str(), -1, &w[0], need);
        w.resize(wcslen(w.c_str()));
        dir = w;
    } else {
        wchar_t profile[MAX_PATH] = {};
        const DWORD n =
            GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            Log::get().note("journal: USERPROFILE is not set, so the journal "
                            "cannot be found. Set d3d11.journal_dir to the "
                            "'Saved Games\\Frontier Developments\\Elite "
                            "Dangerous' folder to use it anyway.");
            return;
        }
        dir = std::wstring(profile) +
              L"\\Saved Games\\Frontier Developments\\Elite Dangerous";
    }

    const DWORD attrs = GetFileAttributesW(dir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        Log::get().note("journal: no folder at the expected place, so the "
                        "game's journal is not being read and the render-state "
                        "heuristics carry everything, as before. Set "
                        "d3d11.journal_dir if your Saved Games folder was "
                        "moved.");
        return;
    }

    // The worker's state. The thread itself starts at the first tick.
    Session* s = new (std::nothrow) Session;
    if (s) {
        s->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!s->wake) {
            delete s;
            s = nullptr;
        }
    }
    if (!s) {
        Log::get().note("journal: could not set up the reader, so the journal "
                        "is not being read and the render-state heuristics "
                        "carry everything, as before.");
        return;
    }
    s->dir = dir;
    s->notBefore = notBefore;
    g_session.store(s);
    // Release: the session's fields above are what the first tick reads once
    // it sees the watcher active.
    g_v.active.store(true, std::memory_order_release);
    Log::get().note("journal: watching the game's own event stream for the "
                    "boundaries it states outright -- gameplay starting "
                    "(LoadGame), on-foot sessions beginning (Disembark), and "
                    "jumps (StartJump, resolved by FSDJump or "
                    "SupercruiseEntry) for the witchspace star fix. Event "
                    "names only; nothing else is read or kept.");
}

void journalWatchTick() {
    if (!g_v.active.load(std::memory_order_acquire)) return;
    if (g_stopped.load()) return;
    Session* const s = g_session.load(std::memory_order_acquire);
    if (!s) return;

    // The worker starts here, on the first frame that asks, from the Present
    // thread -- never from configure, which some loader path could reach.
    if (!s->started.load(std::memory_order_relaxed)) {
        s->started.store(true, std::memory_order_relaxed);
        if (!startWorker(*s)) {
            Log::get().note("journal: could not start the reader thread, so "
                            "the journal is not being read for this session. "
                            "The render-state heuristics carry everything, as "
                            "before.");
            s->exited.store(true, std::memory_order_release);
            g_v.active.store(false, std::memory_order_release);
            return;
        }
    }

    // Take what the worker has published since the last tick. Nothing here
    // waits: a publish in progress is skipped and taken next frame, and
    // nothing here touches a file.
    if (s->mailSeq.load(std::memory_order_acquire) == s->applied) return;
    Published latest;
    {
        std::unique_lock<std::mutex> lock(s->mailMutex, std::try_to_lock);
        if (!lock.owns_lock()) return;
        latest = s->mail;
        s->applied = s->mailSeq.load(std::memory_order_relaxed);
    }
    applyPublished(latest);
}

bool journalWatchActive() { return g_v.active.load(std::memory_order_acquire); }

void journalWatchSetEagerStatus(bool eager) {
    if (g_eager.exchange(eager, std::memory_order_relaxed) == eager) return;
    // The worker may be asleep until a wake-up computed from the old cadence.
    if (Session* s = g_session.load(std::memory_order_acquire)) SetEvent(s->wake);
}

bool journalFssFocusKnown() { return journalWatchActive() && peek(g_v.fssFocusKnown); }
bool journalFssFocus() { return journalWatchActive() && peek(g_v.fssFocus); }
bool journalGuiFocus(uint32_t* focus) {
    if (!journalWatchActive() || !peek(g_v.fssFocusKnown)) return false;
    if (focus) *focus = peek(g_v.guiFocus);
    return true;
}
bool journalSupercruiseKnown() {
    return journalWatchActive() && peek(g_v.supercruiseKnown);
}
bool journalSupercruise() { return journalWatchActive() && peek(g_v.supercruise); }
bool journalGameplay() { return peek(g_v.gameplay); }
uint32_t journalDisembarks() { return peek(g_v.disembarks); }
uint32_t journalEmbarks() { return peek(g_v.embarks); }

bool journalInJumpTunnel() {
    const uint64_t armed = peek(g_v.jumpArmedMs);
    if (!armed) return false;
    // A cancelled hyperspace charge emits no resolving event, so an armed
    // state that outlives any real tunnel expires on its own. Real tunnels
    // run 10-25 seconds after a ~5 second countdown; 45 covers them with
    // room, and bounds how long a cancelled charge could mis-scope a
    // consumer to well under a minute.
    if (stampMs() - armed > 45000) return false;
    // Inside the armed window, the tunnel itself: the Status flag when the
    // game publishes it (about one-second granularity, plenty for an
    // effect that lasts tens of seconds), a countdown-length delay when it
    // does not. StartJump fires at the countdown, and the five seconds
    // before the tunnel are normal space with the same sprite family
    // drawn correctly -- the phase the field caught being pinned.
    if (peek(g_v.fsdJumpKnown)) return peek(g_v.fsdJumpLive);
    return stampMs() - armed > 5500;
}
bool journalOnFootKnown() { return journalWatchActive() && peek(g_v.onFootKnown); }
bool journalOnFoot() { return peek(g_v.onFoot); }
bool journalInMainShipKnown() { return journalWatchActive() && peek(g_v.inMainShipKnown); }
bool journalInMainShip() { return peek(g_v.inMainShip); }
bool journalInFighterKnown() { return journalWatchActive() && peek(g_v.inFighterKnown); }
bool journalInFighter() { return peek(g_v.inFighter); }
bool journalInSrvKnown() { return journalWatchActive() && peek(g_v.inSrvKnown); }
bool journalInSrv() { return peek(g_v.inSrv); }
bool journalInTaxiKnown() { return journalWatchActive() && peek(g_v.inTaxiKnown); }
bool journalInTaxi() { return peek(g_v.inTaxi); }
uint32_t journalStatusSamples() { return peek(g_v.statusSamples); }

void journalWatchShutdown() {
    g_stopped.store(true);
    if (Session* s = g_session.load()) {
        s->stop.store(true, std::memory_order_release);
        SetEvent(s->wake);
    }
}

void journalWatchTestSetWorkHook(JournalWorkHook hook) {
    g_workHook.store(hook, std::memory_order_relaxed);
}

void journalWatchTestSetCadencePercent(uint32_t percent) {
    g_cadencePct.store(percent ? percent : 100, std::memory_order_relaxed);
}

bool journalWatchTestWorkerRunning() {
    const Session* s = g_session.load();
    return s && s->started.load() && !s->exited.load(std::memory_order_acquire);
}

}  // namespace edvr
