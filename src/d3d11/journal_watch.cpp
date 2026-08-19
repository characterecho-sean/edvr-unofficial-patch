#include "journal_watch.h"

#include "../common/timing.h"

#include <windows.h>

#include <cstring>
#include <string>

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
    // Live Flags2 from Status.json: OnFoot is bit 0. `onFootKnown` is false
    // whenever the file lacks a Flags2 field, which is exactly the menu and
    // shutdown states -- Status then carries only "Flags":0.
    bool         onFootKnown = false;
    bool         onFoot = false;
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

// The newest Journal.*.log modified since this process started, or empty.
std::wstring newestJournal() {
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = g_s.dir + L"\\Journal.*.log";
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return std::wstring();
    std::wstring best;
    FILETIME bestTime = {};
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (CompareFileTime(&fd.ftLastWriteTime, &g_s.notBefore) < 0) continue;
        if (best.empty() || CompareFileTime(&fd.ftLastWriteTime, &bestTime) > 0) {
            best = fd.cFileName;
            bestTime = fd.ftLastWriteTime;
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return best;
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
// rewritten by the game about once a second. The single fact taken from it is
// Flags2's OnFoot bit; a file without a Flags2 field -- the menu, shutdown --
// answers "not known", and callers fall back to keys.
void pollStatus() {
    const std::wstring path = g_s.dir + L"\\Status.json";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    bool parsed = false;
    bool sawFlags2 = false;
    uint32_t flags2 = 0;
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
        }
        CloseHandle(f);
    }
    if (parsed) {
        g_s.statusMisses = 0;
        ++g_s.statusSamples;
        g_s.onFootKnown = sawFlags2;
        g_s.onFoot = sawFlags2 && (flags2 & 0x01u) != 0;
    } else if (++g_s.statusMisses >= 3) {
        g_s.onFootKnown = false;
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

void journalWatchConfigure() {
    Config& cfg = Config::get();
    g_s.enabled = cfg.getBool("d3d11.journal_watch", true);
    if (!g_s.enabled) return;

    GetSystemTimeAsFileTime(&g_s.notBefore);
    // Half a minute of slack: the journal for THIS process is created around
    // the moment this code runs, and file times are not promised to be
    // ordered against ours to the millisecond.
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
    if (!dueMs(s.pollMs, kPollMs)) return;
    s.pollMs = stampMs();

    pollStatus();

    // Find or refresh the file being tailed.
    if (s.handle == INVALID_HANDLE_VALUE || dueMs(s.reglobMs, kReglobMs)) {
        s.reglobMs = stampMs();
        const std::wstring newest = newestJournal();
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
            // A new file replays from the top: its early lines are this
            // session's LoadGame, which is exactly the state being rebuilt.
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
    return true;
}
bool journalOnFootKnown() { return g_s.active && g_s.onFootKnown; }
bool journalOnFoot() { return g_s.onFoot; }
uint32_t journalStatusSamples() { return g_s.statusSamples; }

void journalWatchShutdown() { closeFile(); }

}  // namespace edvr
