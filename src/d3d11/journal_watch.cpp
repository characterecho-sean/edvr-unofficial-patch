#include "journal_watch.h"

#include "../common/timing.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "../common/config.h"
#include "../common/log.h"

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

// Consecutive file-op failures before the watcher retires for the session.
constexpr uint32_t kMaxFaults = 8;

// The largest slice read per poll. Journal bursts (Materials, Statistics at
// login) run tens of kilobytes; one slice a second keeps up with anything the
// game writes and bounds the frame cost either way.
constexpr uint32_t kReadChunk = 64 * 1024;

struct State {
    bool         enabled = true;
    bool         active = false;
    bool         gameplay = false;
    uint32_t     disembarks = 0;
    uint32_t     embarks = 0;
    // When a jump tunnel MAY be on screen: armed at StartJump, cleared by
    // the event that resolves it (FSDJump for hyperspace, SupercruiseEntry
    // for the other branch -- StartJump fires for both, and telling them
    // apart by NAME keeps the names-only posture). A cancelled charge
    // resolves with neither, which is what the expiry in the accessor is
    // for. Zero means not armed.
    uint64_t     jumpArmedMs = 0;
    // Status Flags bit 30, the FSD jump itself. StartJump fires at the
    // COUNTDOWN, five seconds before any tunnel exists, and the field found
    // the difference immediately: the forming-wormhole sprite is the same
    // family, world-anchored, and was being pinned on the pad. The flag is
    // what narrows the window to the tunnel; `known` is false when Status
    // lacks the field, and the accessor then falls back to a countdown-
    // length delay after StartJump.
    bool         fsdJumpKnown = false;
    bool         fsdJumpLive = false;
    // Live Flags2 from Status.json: OnFoot is bit 0. `onFootKnown` is false
    // whenever the file lacks a Flags2 field, which is exactly the menu and
    // shutdown states -- Status then carries only "Flags":0.
    bool         onFootKnown = false;
    bool         onFoot = false;
    // Which vehicle, for the camera-cycle ring. OnFoot wins over the vehicle
    // bits: disembarking inside a ship leaves both true for a moment, and the
    // commander is the thing being asked about.
    JournalVehicle vehicle = JournalVehicle::Unknown;
    // GuiFocus from Status.json: 9 is the Full System Scanner. The game
    // states the MODE outright -- entry and exit by any path (keybind,
    // ESC, an interdiction yanking the player out of supercruise) all
    // land here, which no keypress watcher can promise. Supercruise is
    // Flags bit 4, read to qualify FSS-key presses (the key does nothing
    // outside supercruise).
    bool         fssFocusKnown = false;
    bool         fssFocus = false;
    bool         supercruiseKnown = false;
    bool         supercruise = false;
    uint64_t     statusMs = 0;   // Status.json's own clock
    bool         eagerStatus = false;
    uint32_t     statusSamples = 0;   // successful Status.json parses
    uint64_t     pollMs = 0;     // last poll
    uint64_t     reglobMs = 0;   // last directory re-glob
    uint32_t     faults = 0;
    bool         faultsNoted = false;
    std::wstring dir;
    std::wstring file;        // the journal currently tailed
    HANDLE       handle = INVALID_HANDLE_VALUE;
    uint64_t     offset = 0;
    // Files older than EDVR's own start are the PREVIOUS session's journal:
    // replaying one would fire last session's boundaries into this one.
    FILETIME     notBefore = {};
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
};
State g_s;

void closeFile() {
    if (g_s.handle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_s.handle);
        g_s.handle = INVALID_HANDLE_VALUE;
    }
    g_s.offset = 0;
    g_s.carryLen = 0;
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
std::wstring newestJournal(bool& ours) {
    // The walk collects; journalPickNewest decides. The decision is a pure
    // function over times so it can be put in a table -- see its header.
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = g_s.dir + L"\\Journal.*.log";
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

    const JournalPick pick = journalPickNewest(
        creation.data(), write.data(), names.size(), asU64(g_s.notBefore));
    ours = pick.ours;
    return pick.index < 0 ? std::wstring()
                          : names[static_cast<size_t>(pick.index)];
}

void onEvent(const char* name, uint32_t len) {
    // The five that matter. Everything else in the stream is somebody else's
    // business, and not keeping it is the privacy posture: names only, and
    // only these names do anything.
    if (len == 8 && memcmp(name, "LoadGame", 8) == 0) {
        if (!g_s.gameplay) {
            g_s.gameplay = true;
            Log::get().note("journal: LoadGame -- gameplay has started, so the "
                            "camera hotkeys mean the camera now rather than a "
                            "menu.");
        }
    } else if (len == 9 && memcmp(name, "Disembark", 9) == 0) {
        ++g_s.disembarks;
    } else if (len == 6 && memcmp(name, "Embark", 6) == 0) {
        ++g_s.embarks;
    } else if (len == 8 && memcmp(name, "Shutdown", 8) == 0) {
        // The game is leaving. Gameplay ends with it; a relaunch gets a new
        // journal and a fresh LoadGame.
        g_s.gameplay = false;
        g_s.jumpArmedMs = 0;
    } else if (len == 9 && memcmp(name, "StartJump", 9) == 0) {
        g_s.jumpArmedMs = stampMs();
    } else if (len == 7 && memcmp(name, "FSDJump", 7) == 0) {
        // Hyperspace arrival: the tunnel is over.
        g_s.jumpArmedMs = 0;
    } else if (len == 16 && memcmp(name, "SupercruiseEntry", 16) == 0) {
        // StartJump's other branch: this was a supercruise transition, not a
        // hyperspace tunnel.
        g_s.jumpArmedMs = 0;
    }
    // Embark / Touchdown / Liftoff are recognised by name here should a
    // consumer ever need them; today the ones above carry the features.
}

const char kEventTok[] = "\"event\":\"";
constexpr uint32_t kEventTokLen = sizeof(kEventTok) - 1;

void scanRange(const char* p, uint32_t n) {
    for (uint32_t i = 0; i + kEventTokLen < n; ++i) {
        if (memcmp(p + i, kEventTok, kEventTokLen) != 0) continue;
        const uint32_t start = i + kEventTokLen;
        uint32_t end = start;
        while (end < n && p[end] != '"' && end - start < 40) ++end;
        if (end < n && p[end] == '"') onEvent(p + start, end - start);
        i = end;
    }
}

// Status.json, reread whole on the same cadence: it is a few hundred bytes,
// rewritten by the game about once a second. Two facts are taken from it:
// Flags2's OnFoot bit, and Flags' FSD-jump bit (30). A file without the
// field -- the menu, shutdown -- answers "not known", and callers fall back
// to keys and delays respectively.
void pollStatus() {
    const std::wstring path = g_s.dir + L"\\Status.json";
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
        g_s.statusMisses = 0;
        ++g_s.statusSamples;
        g_s.onFootKnown = sawFlags2;
        g_s.onFoot = sawFlags2 && (flags2 & 0x01u) != 0;
        // Bits 24 main ship, 25 fighter, 26 SRV.
        if (g_s.onFoot) {
            g_s.vehicle = JournalVehicle::OnFoot;
        } else if (!sawFlags) {
            g_s.vehicle = JournalVehicle::Unknown;
        } else if ((flags & 0x04000000u) != 0) {
            g_s.vehicle = JournalVehicle::Srv;
        } else if ((flags & 0x02000000u) != 0) {
            g_s.vehicle = JournalVehicle::Fighter;
        } else if ((flags & 0x01000000u) != 0) {
            g_s.vehicle = JournalVehicle::Ship;
        } else {
            g_s.vehicle = JournalVehicle::Unknown;
        }
        // Bit 30 of Flags: the FSD jump itself -- the tunnel, not the
        // countdown before it. The distinction is what scopes the witchspace
        // star fix off the forming-wormhole phase, where the game still
        // positions the same sprite family correctly in the world.
        g_s.fsdJumpKnown = sawFlags;
        g_s.fsdJumpLive = sawFlags && (flags & 0x40000000u) != 0;
        g_s.supercruiseKnown = sawFlags;
        g_s.supercruise = sawFlags && (flags & 0x10u) != 0;
        const bool fss = sawGui && gui == 9;
        if (fss != g_s.fssFocus) {
            Log::get().note(fss ? "status: GuiFocus 9 -- the game says the "
                                  "player is in the Full System Scanner."
                                : "status: the game says FSS focus ended.");
        }
        g_s.fssFocusKnown = sawGui;
        g_s.fssFocus = fss;
    } else if (++g_s.statusMisses >= 3) {
        g_s.onFootKnown = false;
        g_s.vehicle = JournalVehicle::Unknown;
        g_s.fsdJumpKnown = false;
        g_s.fssFocusKnown = false;
        g_s.fssFocus = false;
        g_s.supercruiseKnown = false;
    }
}

// Scan a buffer for `"event":"NAME"`, with a small carry so a token split
// across chunk boundaries is still found.
void scanEvents(const char* data, uint32_t len) {
    // Stitch the carry to the front so a split token reassembles.
    char stitched[sizeof(g_s.carry) + 256];
    uint32_t stitchedLen = 0;
    if (g_s.carryLen) {
        memcpy(stitched, g_s.carry, g_s.carryLen);
        const uint32_t take = len < 256 ? len : 256;
        memcpy(stitched + g_s.carryLen, data, take);
        stitchedLen = g_s.carryLen + take;
    }
    if (stitchedLen) scanRange(stitched, stitchedLen);
    scanRange(data, len);

    // Keep the tail as the next carry. Anything already scanned in `stitched`
    // that also sits in `data`'s head double-scans harmlessly: LoadGame and
    // Shutdown are idempotent and a Disembark token cannot span BOTH the old
    // carry and the new tail.
    const uint32_t keep =
        len < sizeof(g_s.carry) ? len : static_cast<uint32_t>(sizeof(g_s.carry));
    memcpy(g_s.carry, data + (len - keep), keep);
    g_s.carryLen = keep;
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
    Config& cfg = Config::get();
    g_s.enabled = cfg.getBool("d3d11.journal_watch", true);
    if (!g_s.enabled) return;

    GetSystemTimeAsFileTime(&g_s.notBefore);
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
    t.LowPart = g_s.notBefore.dwLowDateTime;
    t.HighPart = g_s.notBefore.dwHighDateTime;
    t.QuadPart -= 30ull * 10000000ull;
    g_s.notBefore.dwLowDateTime = t.LowPart;
    g_s.notBefore.dwHighDateTime = t.HighPart;

    const std::string dirUtf8 = cfg.getString("d3d11.journal_dir", "");
    if (!dirUtf8.empty()) {
        const int need = MultiByteToWideChar(CP_UTF8, 0, dirUtf8.c_str(), -1,
                                             nullptr, 0);
        std::wstring w(need > 0 ? need : 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, dirUtf8.c_str(), -1, &w[0], need);
        w.resize(wcslen(w.c_str()));
        g_s.dir = w;
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
        g_s.dir = std::wstring(profile) +
                  L"\\Saved Games\\Frontier Developments\\Elite Dangerous";
    }

    const DWORD attrs = GetFileAttributesW(g_s.dir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        Log::get().note("journal: no folder at the expected place, so the "
                        "game's journal is not being read and the render-state "
                        "heuristics carry everything, as before. Set "
                        "d3d11.journal_dir if your Saved Games folder was "
                        "moved.");
        return;
    }
    g_s.active = true;
    Log::get().note("journal: watching the game's own event stream for the "
                    "boundaries it states outright -- gameplay starting "
                    "(LoadGame), on-foot sessions beginning (Disembark), and "
                    "jumps (StartJump, resolved by FSDJump or "
                    "SupercruiseEntry) for the witchspace star fix. Event "
                    "names only; nothing else is read or kept.");
}

void journalWatchTick() {
    State& s = g_s;
    if (!s.active) return;
    // dueMs, not elapsedMs: the counter this replaced started at 0 meaning
    // "poll on the first frame", and a stamp of 0 read as "never due" would
    // have retired the watcher before it ever ran -- silently, because the
    // only write to the stamp is the line below, inside the branch it gates.
    // Status.json rides its own clock: ~1 KB reread, normally on the
    // journal's cadence, but at 100 ms when a consumer asked for low
    // latency (the FSS theater's mode gate -- a screen that engages a
    // second late is a screen the player watched arrive).
    constexpr uint64_t kStatusEagerMs = 100;
    if (dueMs(s.statusMs, s.eagerStatus ? kStatusEagerMs : kPollMs)) {
        s.statusMs = stampMs();
        pollStatus();
    }

    if (!dueMs(s.pollMs, kPollMs)) return;
    s.pollMs = stampMs();

    // Find or refresh the file being tailed.
    if (s.handle == INVALID_HANDLE_VALUE || dueMs(s.reglobMs, kReglobMs)) {
        s.reglobMs = stampMs();
        bool ours = false;
        const std::wstring newest = newestJournal(ours);
        if (!newest.empty() && newest != s.file) {
            closeFile();
            s.file = newest;
            s.handle = CreateFileW((s.dir + L"\\" + s.file).c_str(),
                                   GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE |
                                       FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (s.handle == INVALID_HANDLE_VALUE) {
                if (++s.faults > kMaxFaults) goto retire;
                s.file.clear();
                return;
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
                    // lines an hour and the 4 MB cap inside three hours, which is
                    // the instrument destroying the evidence for the third time
                    // in this codebase. Uncharged, the watcher never retires
                    // either, because kMaxFaults is what retires it.
                    const DWORD err = GetLastError();
                    closeFile();
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
                    return;
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
        if (s.handle == INVALID_HANDLE_VALUE) return;   // nothing yet
    }

    // Read whatever has appeared since last time.
    {
        LARGE_INTEGER pos;
        pos.QuadPart = static_cast<LONGLONG>(s.offset);
        if (!SetFilePointerEx(s.handle, pos, nullptr, FILE_BEGIN)) {
            if (++s.faults > kMaxFaults) goto retire;
            return;
        }
        static char buf[kReadChunk];
        DWORD got = 0;
        if (!ReadFile(s.handle, buf, kReadChunk, &got, nullptr)) {
            if (++s.faults > kMaxFaults) goto retire;
            return;
        }
        if (got > 0) {
            s.offset += got;
            scanEvents(buf, got);
            s.faults = 0;
        }
    }
    return;

retire:
    if (!s.faultsNoted) {
        s.faultsNoted = true;
        Log::get().note("journal: %u file errors in a row, so the journal is "
                        "not being read for the rest of this session. The "
                        "render-state heuristics carry everything, as before.",
                        s.faults);
    }
    closeFile();
    s.active = false;
}

bool journalWatchActive() { return g_s.active; }

void journalWatchSetEagerStatus(bool eager) { g_s.eagerStatus = eager; }

bool journalFssFocusKnown() { return g_s.active && g_s.fssFocusKnown; }
bool journalFssFocus() { return g_s.active && g_s.fssFocus; }
bool journalSupercruiseKnown() {
    return g_s.active && g_s.supercruiseKnown;
}
bool journalSupercruise() { return g_s.active && g_s.supercruise; }

JournalVehicle journalVehicle() {
    return g_s.active ? g_s.vehicle : JournalVehicle::Unknown;
}
bool journalGameplay() { return g_s.gameplay; }
uint32_t journalDisembarks() { return g_s.disembarks; }
uint32_t journalEmbarks() { return g_s.embarks; }

bool journalInJumpTunnel() {
    if (!g_s.jumpArmedMs) return false;
    // A cancelled hyperspace charge emits no resolving event, so an armed
    // state that outlives any real tunnel expires on its own. Real tunnels
    // run 10-25 seconds after a ~5 second countdown; 45 covers them with
    // room, and bounds how long a cancelled charge could mis-scope a
    // consumer to well under a minute.
    if (stampMs() - g_s.jumpArmedMs > 45000) {
        g_s.jumpArmedMs = 0;
        return false;
    }
    // Inside the armed window, the tunnel itself: the Status flag when the
    // game publishes it (about one-second granularity, plenty for an
    // effect that lasts tens of seconds), a countdown-length delay when it
    // does not. StartJump fires at the countdown, and the five seconds
    // before the tunnel are normal space with the same sprite family
    // drawn correctly -- the phase the field caught being pinned.
    if (g_s.fsdJumpKnown) return g_s.fsdJumpLive;
    return stampMs() - g_s.jumpArmedMs > 5500;
}
bool journalOnFootKnown() { return g_s.active && g_s.onFootKnown; }
bool journalOnFoot() { return g_s.onFoot; }
uint32_t journalStatusSamples() { return g_s.statusSamples; }

void journalWatchShutdown() { closeFile(); }

}  // namespace edvr
