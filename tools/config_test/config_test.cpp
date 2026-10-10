// config_test -- runs the real parser over the real shipped edvr.ini.
//
// The file users edit is the one asserted here. Two properties of the parser
// that edvr.ini's own layout depends on are proven rather than assumed:
//
//   1. a section header may REPEAT -- edvr.ini opens with the settings most
//      people touch under [fix] and [hotkey], then returns to both further
//      down for the Explorer Cam block. If a repeat were a parse error, or
//      silently discarded everything after it, half the file would do
//      nothing while still looking perfectly valid in an editor.
//   2. a key set twice takes the LAST value, because parse() assigns into a
//      flat section.key map.
//
// Both were originally read out of config.cpp rather than observed, which is
// the wrong order for this project: every symptom of either being false
// appears in the game rather than here.
//
// The rest are regressions with history. The inline-comment and yes/no cases
// were real bugs, and the BOM case files every setting in the file under the
// wrong section while the file still looks fine.
//
// Four more (2026-09-29) are about threads, the log and the file's sharing, and
// read the real log file back rather than counting calls:
//
//   - a reader thread hammering the getters while the main thread rewrites the
//     ini and reloads it, so a lock that is missing, or held only around the
//     lookup and not the copy, shows as a wrong value or a crash;
//   - a malformed value is said once per key per successful parse, however
//     often it is read -- it was said on every read, 90 to 180 lines a second
//     for a key read each frame;
//   - getFloat takes the whole value and only a finite one;
//   - a reload leaves edvr.ini open to being replaced, renamed or deleted (the
//     read shares DELETE): the menu saves by renaming a temp file over it with
//     POSIX semantics, and a read that did not share DELETE held that off while
//     the reload had the file open.
//
// Usage: config_test.exe <dir containing edvr.ini> [scratch dir]
//        config_test.exe --ininame-cases <scratch dir>   (its own child; see iniNameFixturesIsolated)
#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

#include "../../src/common/config.h"
#include "../../src/common/ini_name.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/common/temporal_mode.h"
#include "../../src/common/log.h"
#include "../../src/d3d11/explorer_cam_follow_core.h"   // the Explorer Cam trims' and smoothing defaults: header-only, no link
#include "config_contract_gen.h"   // build\gen: the tables the DLLs register, from the real edvr.ini

using namespace edvr;

static int g_fails = 0;

static void ok(const char* what) { printf("  ok    %s\n", what); }

static void fail(const char* what, const std::string& detail) {
    printf("  FAIL  %s -- %s\n", what, detail.c_str());
    ++g_fails;
}

static void check(bool condition, const char* what, const std::string& detail = std::string()) {
    if (condition) ok(what);
    else fail(what, detail.empty() ? std::string("the condition did not hold") : detail);
}

static void expectStr(const char* key, const char* want, const char* what) {
    const std::string got = Config::get().getString(key, "<unset>");
    if (got == want) ok(what);
    else fail(what, std::string(key) + " = \"" + got + "\", wanted \"" + want + "\"");
}

static void expectInt(const char* key, int want, const char* what) {
    const int got = Config::get().getInt(key, -999999);
    if (got == want) ok(what);
    else fail(what, std::string(key) + " = " + std::to_string(got) +
                        ", wanted " + std::to_string(want));
}

static void expectFloat(const char* key, float want, const char* what) {
    const float got = Config::get().getFloat(key, -999999.0f);
    const float d = got > want ? got - want : want - got;
    if (d < 0.0001f) ok(what);
    else fail(what, std::string(key) + " = " + std::to_string(got) +
                        ", wanted " + std::to_string(want));
}

static void expectBool(const char* key, bool want, const char* what) {
    // Both defaults, so a key that is absent entirely cannot pass by matching
    // the default that happens to be wanted.
    const bool a = Config::get().getBool(key, false);
    const bool b = Config::get().getBool(key, true);
    if (a == want && b == want) ok(what);
    else fail(what, std::string(key) + " = " + (a ? "true" : "false") + "/" +
                        (b ? "true" : "false") + ", wanted " +
                        (want ? "true" : "false") + " from both defaults");
}

// Deliberately not `std::wstring(std::string(p).begin(), std::string(p).end())`.
// That spells TWO temporaries and takes begin() from one and end() from the
// other, so the range is whatever the gap between two stack objects happens to
// be -- which is how the first run of this test died inside the constructor,
// before a single printf.
static std::wstring widen(const char* p) {
    std::wstring out;
    for (const char* c = p; *c; ++c) out.push_back(static_cast<wchar_t>(*c));
    return out;
}

// A scratch ini written from a literal, for the cases the real file cannot
// contain without being wrong.
static bool writeIni(const std::wstring& dir, const char* body,
                     const wchar_t* leaf = L"edvr.ini") {
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\" + leaf;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(f, body, static_cast<DWORD>(strlen(body)), &written, nullptr);
    CloseHandle(f);
    return true;
}

// The text of a file under the repo root, or empty: for the checks that hold
// the shipped ini and the code that reads it to one answer.
static std::string readRepoFile(const std::wstring& root, const wchar_t* relative) {
    std::string out;
    HANDLE f = CreateFileW((root + L"\\" + relative).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
    CloseHandle(f);
    return out;
}

// The literal a getString call falls back to for `key`: its second argument.
// "<no such read>" when the call is not in the source, so a rename fails loudly.
static std::string codeFallbackOf(const std::string& source, const char* key) {
    const std::string needle = std::string("getString(\"") + key + "\", \"";
    const size_t at = source.find(needle);
    if (at == std::string::npos) return "<no such read>";
    const size_t begin = at + needle.size();
    const size_t end = source.find('"', begin);
    return end == std::string::npos ? "<unterminated>" : source.substr(begin, end - begin);
}

// writeIni, then a last-write time no earlier write has carried.
//
// reloadIfChanged() decides by comparing last-write times, and two writes inside
// one clock tick -- 15 ms unless something has raised the timer rate -- carry the
// same one. A loop that rewrites the file and reloads would then skip most of
// its reloads without saying so, and a test of what a reload does to a reader
// would be testing almost nothing. Each call stamps a time one second on from
// the last, so every reload that follows is a parse.
static bool rewriteIni(const std::wstring& dir, const std::string& body,
                       const wchar_t* leaf = L"edvr.ini") {
    if (!writeIni(dir, body.c_str(), leaf)) return false;
    static ULONGLONG stamp = 0;
    if (!stamp) {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        stamp = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    }
    stamp += 10000000ull;  // 100 ns ticks: one second
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(stamp & 0xFFFFFFFFull);
    ft.dwHighDateTime = static_cast<DWORD>(stamp >> 32);
    HANDLE f = CreateFileW((dir + L"\\" + leaf).c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const BOOL stamped = SetFileTime(f, nullptr, nullptr, &ft);
    CloseHandle(f);
    return stamped != FALSE;
}

// Every edvr_<tag>_*.log in `dir`, gone: so "the newest" below is provably THIS
// run's, and the scratch directory does not grow with every build.
static void deleteLogs(const std::wstring& dir, const wchar_t* tag) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\edvr_" + tag + L"_*.log").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// The newest edvr_<tag>_*.log in `dir`, whole; empty if there is none. Close the
// log first: it is written by a flusher thread and only close() drains it.
static std::string readNewestLog(const std::wstring& dir, const wchar_t* tag) {
    std::string body;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\edvr_" + tag + L"_*.log").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return body;
    std::wstring newest = fd.cFileName;
    while (FindNextFileW(h, &fd)) newest = fd.cFileName;
    FindClose(h);
    HANDLE f = CreateFileW((dir + L"\\" + newest).c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return body;
    char chunk[65536];
    DWORD got = 0;
    while (ReadFile(f, chunk, sizeof(chunk), &got, nullptr) && got) body.append(chunk, got);
    CloseHandle(f);
    return body;
}

static int countOf(const std::string& haystack, const char* needle) {
    int n = 0;
    for (size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + 1)) {
        ++n;
    }
    return n;
}

// --- getFloat: the whole value, and a finite one (2026-09-29) --------------
//
// strtof stops at the first character it cannot use and accepts "nan" and
// "inf". getFloat asked only that SOMETHING parsed, so "1.5x" read as 1.5 and
// "2,75" as 2 -- the truncation getInt was cured of -- and a NaN walked past
// every range test a caller wrote, because it compares false against all of
// them. A refused value is the caller's default, which is what expectFloat's
// -999999 sentinel stands for.
static void floatCases(const std::wstring& scratch) {
    static const char kIni[] =
        "[flt]\r\n"
        "nan = nan\r\n"
        "inf = inf\r\n"
        "neginf = -inf\r\n"
        "overflow = 1e999\r\n"      // strtof answers infinity, not an error
        "junk = 1.5x\r\n"
        "comma = 2,75\r\n"          // the comma locale the comment on getFloat names
        "good = 1.5\r\n"
        "neg = -0.25\r\n"
        "whole = 2\r\n"
        "exponent = 1e-3\r\n";
    if (!writeIni(scratch, kIni)) {
        fail("float scratch ini", "could not write it");
        return;
    }
    Config::get().init(scratch);
    expectFloat("flt.nan", -999999.0f, "getFloat refuses nan");
    expectFloat("flt.inf", -999999.0f, "...and inf");
    expectFloat("flt.neginf", -999999.0f, "...and -inf");
    expectFloat("flt.overflow", -999999.0f, "...and a value too big for a float");
    expectFloat("flt.junk", -999999.0f, "...and a number with a letter after it");
    expectFloat("flt.comma", -999999.0f, "...and a decimal written with a comma");
    expectFloat("flt.good", 1.5f, "getFloat still reads a plain decimal");
    expectFloat("flt.neg", -0.25f, "...a negative one");
    expectFloat("flt.whole", 2.0f, "...a whole number");
    expectFloat("flt.exponent", 0.001f, "...and an exponent");
}

// --- a malformed value is said once per parse, however often it is read -----
//
// The getters noted it on EVERY read. A key read each frame wrote the same line
// 90 to 180 times a second until log.max_mb, and then the flight logged
// nothing. The property is read off the real log file, because a counter would
// only prove that a counter was incremented.
//
// The same session covers the config audit's queue, whose flush now takes an
// atomic flag and a lock instead of reading a vector: findings raised before
// the log opens are written when it does, exactly once, and a reload that
// finds the same lines does not write them again.
//
// Three parses, one log:
//   1. malformed values, log CLOSED for the first hundred reads, then open.
//      Nothing can be written while it is closed, and that must not use up the
//      key's one note -- most keys are first read before Log::open.
//   2. the same values again, rewritten: a new parse, so one more note.
//   3. valid values: nothing to say.
// So each malformed key appears twice in the file, not 0, 1 or 200.
static void noteCases(const std::wstring& scratch) {
    static const char* kKnown[] = {"experimental.zeta", "once.flag", "once.count",
                                   "once.scale", "once.range", "once.rangebad"};
    static const char* kMoved[][3] = {{"fix.zeta", "experimental.zeta", ""}};
    const std::string first =
        "[once]\r\n"
        "flag = maybe\r\n"          // getBool
        "count = 4w\r\n"            // getInt
        "scale = 1,5\r\n"           // getFloat
        "range = 999\r\n"           // getIntInRange, past the top
        "rangebad = twelve\r\n"     // getIntInRange, not a number
        "[fix]\r\n"
        "zeta = 7\r\n"              // a moved key: the audit says so
        "mystery2 = 9\r\n";         // a key nothing reads: the audit says so
    const std::string second = first + "# written again, same values\r\n";
    const std::string valid =
        "[once]\r\n"
        "flag = yes\r\n"
        "count = 4\r\n"
        "scale = 1.5\r\n"
        "range = 7\r\n"
        "rangebad = 8\r\n";

    Config& cfg = Config::get();
    // Reads every key 100 times and counts the answers that are not the ones a
    // caller must get: the default for a malformed value, the clamp for an
    // out-of-range one. Saying less must not change what is returned.
    auto readRound = [&cfg](bool valid_) {
        int wrong = 0;
        for (int i = 0; i < 100; ++i) {
            // A default that is NOT the answer, so a value read as absent shows.
            if (cfg.getBool("once.flag", !valid_) != true) ++wrong;
            if (cfg.getInt("once.count", 5) != (valid_ ? 4 : 5)) ++wrong;
            if (cfg.getFloat("once.scale", 0.5f) != (valid_ ? 1.5f : 0.5f)) ++wrong;
            if (cfg.getIntInRange("once.range", 5, 1, 10) != (valid_ ? 7 : 10)) ++wrong;
            if (cfg.getIntInRange("once.rangebad", 5, 1, 10) != (valid_ ? 8 : 5)) ++wrong;
        }
        return wrong;
    };

    Log::get().close();                 // whatever an earlier case left open
    deleteLogs(scratch, L"noteonce");
    cfg.setAuditTables(kKnown, 6, kMoved, 1);
    auto finish = [&]() {
        Log::get().close();
        cfg.setAuditTables(nullptr, 0, nullptr, 0);
    };

    if (!rewriteIni(scratch, first)) {
        fail("note cases", "could not write the scratch ini");
        finish();
        return;
    }
    cfg.init(scratch);                  // parse 1: the audit's findings queue
    int wrong = readRound(false);       // log closed: nothing to write, nothing spent
    if (!Log::get().open(scratch, L"noteonce")) {
        fail("note cases", "the log would not open in the scratch dir");
        finish();
        return;
    }
    wrong += readRound(false);          // parse 1, log open
    if (!rewriteIni(scratch, second) || !cfg.reloadIfChanged()) {
        fail("note cases", "the second write did not reload");
        finish();
        return;
    }
    wrong += readRound(false);          // parse 2
    if (!rewriteIni(scratch, valid) || !cfg.reloadIfChanged()) {
        fail("note cases", "the third write did not reload");
        finish();
        return;
    }
    wrong += readRound(true);           // parse 3: valid, so silent
    finish();

    if (wrong) {
        fail("note cases", std::to_string(wrong) + " reads returned something other than "
                           "the default, the clamp or the value");
    } else {
        ok("saying a malformed value once changes nothing a getter returns");
    }

    const std::string body = readNewestLog(scratch, L"noteonce");
    if (body.empty()) {
        fail("note cases", "could not read the log back");
        return;
    }
    static const struct { const char* needle; int want; const char* what; } kNeeds[] = {
        {"once.flag = \"maybe\"", 2,
         "a malformed yes/no is noted once per parse, not once per read"},
        {"once.count = \"4w\"", 2, "...a malformed integer"},
        {"once.scale = \"1,5\"", 2, "...a malformed float"},
        {"once.range = 999 is outside", 2, "...an out-of-range bounded integer"},
        {"once.rangebad = \"twelve\"", 2, "...a malformed bounded integer"},
        {"fix.zeta has moved to experimental.zeta", 1,
         "an audit finding raised before the log opened is written once, and not "
         "again by a reload"},
        {"does not read: fix.mystery2", 1, "...and so is the audit's dead-line note"},
    };
    for (const auto& n : kNeeds) {
        const int got = countOf(body, n.needle);
        if (got == n.want) ok(n.what);
        else fail(n.what, std::string("\"") + n.needle + "\" appears " +
                              std::to_string(got) + " times in the log, wanted " +
                              std::to_string(n.want));
    }
}

// --- reads that race a reload (2026-09-29) ----------------------------------
//
// parse() swaps a freshly parsed map in and frees the old one. The render
// thread does that whenever the ini's write time moves -- an in-VR menu write
// does it -- while the OpenXR owner thread's deferred frame end reads
// advanced.app_gpu_timing on its own. With no lock a read that straddled the
// swap walked nodes that were being freed. And getString called the audit's
// flush, which mutated a shared vector from whichever thread happened to read.
//
// The reader hammers four keys and a fifth that is never present; the main
// thread alternates the file between two contents and reloads each time. Every
// key has one of two legal values (the bool has one: both contents spell it
// false, and the reader's default is true, so an absent key reads as true).
// Anything else -- a default where a value should be, a torn string, an access
// violation -- is the failure. The outcome for a correct build is
// deterministic: the reader either only ever sees a whole map, or it does not.
struct RaceReader {
    std::atomic<bool> stop{false};
    std::atomic<long> reads{0};
    // Written by the reader thread alone; the main thread reads them after join().
    long        bad = 0;
    std::string firstBad;
};

static void raceReaderMain(RaceReader* r) {
    Config& c = Config::get();
    long n = 0;
    auto flag = [r](const std::string& what) {
        if (++r->bad == 1) r->firstBad = what;
    };
    while (!r->stop.load(std::memory_order_relaxed)) {
        if (c.getBool("race.flag", true)) flag("race.flag read true: the key vanished");
        const int count = c.getInt("race.count", -1);
        if (count != 7 && count != 9) flag("race.count = " + std::to_string(count));
        const float scale = c.getFloat("race.scale", -1.0f);
        if (scale != 1.5f && scale != 2.5f) flag("race.scale = " + std::to_string(scale));
        const std::string name = c.getString("race.name", "<none>");
        if (name != "alpha" && name != "bravo") flag("race.name = \"" + name + "\"");
        if (c.getInt("race.absent", -1) != -1) flag("race.absent found a value");
        r->reads.store(++n, std::memory_order_relaxed);
    }
}

static void raceCase(const std::wstring& scratch) {
    const auto content = [](const char* flag, int count, const char* scale, const char* name) {
        std::string s = "[race]\r\n";
        s += std::string("flag = ") + flag + "\r\n";
        s += "count = " + std::to_string(count) + "\r\n";
        s += std::string("scale = ") + scale + "\r\n";
        s += std::string("name = ") + name + "\r\n";
        // A deeper tree takes longer to walk and to free, which is what widens
        // the window that the lock has to close.
        for (int i = 0; i < 64; ++i) s += "pad" + std::to_string(i) + " = " + name + "\r\n";
        return s;
    };
    const std::string a = content("off", 7, "1.5", "alpha");
    const std::string b = content("no", 9, "2.5", "bravo");

    if (!rewriteIni(scratch, a)) {
        fail("config race", "could not write the scratch ini");
        return;
    }
    Config::get().init(scratch);

    RaceReader reader;
    std::thread thread(raceReaderMain, &reader);

    // Do not start reloading until the reader is demonstrably running: a thread
    // that has not been scheduled yet would make the loop below a test of nothing.
    const ULONGLONG t0 = GetTickCount64();
    while (reader.reads.load() < 1000 && GetTickCount64() - t0 < 5000) Sleep(1);
    const bool started = reader.reads.load() >= 1000;

    int  reloads = 0;
    int  unparsed = 0;
    bool writeFailed = false;
    if (started) {
        const ULONGLONG t1 = GetTickCount64();
        while (reloads < 300 && GetTickCount64() - t1 < 1500) {
            if (!rewriteIni(scratch, (reloads & 1) ? a : b)) {
                writeFailed = true;
                break;
            }
            if (!Config::get().reloadIfChanged()) ++unparsed;
            ++reloads;
        }
    }
    reader.stop.store(true);
    thread.join();

    if (!started) {
        fail("config race", "the reader thread did not get going within 5 s");
    } else if (writeFailed) {
        fail("config race", "could not rewrite the scratch ini");
    } else if (reloads < 20) {
        fail("config race", "only " + std::to_string(reloads) + " reloads ran in 1.5 s; "
                            "too few to say anything about a race");
    } else if (unparsed) {
        fail("config race", std::to_string(unparsed) + " of " + std::to_string(reloads) +
                            " reloads found the file unchanged, so they never parsed");
    } else if (reader.bad) {
        fail("config race", std::to_string(reader.bad) + " bad reads; the first: " +
                            reader.firstBad);
    } else {
        ok("reads racing reloads only ever see one whole map or the other");
    }
    printf("  info  %ld reads raced %d reloads\n", reader.reads.load(), reloads);
}

// --- a reload leaves the file open to being replaced (2026-09-29) --------------
//
// Config's read of edvr.ini opened it with FILE_SHARE_READ | FILE_SHARE_WRITE --
// no FILE_SHARE_DELETE -- so for as long as a reload held the file (tens of
// microseconds) nothing could delete it, rename it or replace it with POSIX
// rename semantics, and the in-headset menu, which saves by renaming a temp file
// over edvr.ini and asks for a reload straight after, could meet that hold on its
// very next save.
//
// What this proves is the share mode; the replace itself is installer_test's
// (its atomic-write cases replace a file under a reader that opens it this way).
// iniedit's replace is a POSIX-semantics rename, which goes through under a
// handle that shares DELETE and is refused under one that does not; the classic
// MoveFileExW it falls back to is refused while ANY handle to the target is open.
// The share mode is the half of that which lives here.
//
// The read cannot be paused from outside, so a second thread does to it what a
// rename would: it opens the file for DELETE, with every share mode, over and
// over, and counts the refusals that happen wholly inside a reload. It runs only
// inside a reload, because a probe that ran while the test itself rewrote the
// file would collide with the test: `busy` is the handshake that makes that so,
// since a probe thread can be descheduled between opening the file and closing
// it, and the test must not rewrite the file until that probe is finished.
// A correct parse() is never refused. One without FILE_SHARE_DELETE is refused
// whenever a probe lands while it holds the handle, which is hundreds of probes
// over the run; the threshold below is for a scanner that happens to hold the
// freshly written file for a moment, not for the bug.
struct ProbeState {
    std::atomic<bool> stop{false};
    std::atomic<bool> inReload{false};
    std::atomic<bool> busy{false};  // a probe is between its open and its close
    std::atomic<long> inside{0};    // probes that ran wholly inside a reload
    std::atomic<long> refused{0};   // ...and were refused with a sharing violation
    std::atomic<long> other{0};     // ...and failed some other way
};

static void probeMain(const std::wstring* path, ProbeState* st) {
    while (!st->stop.load()) {
        // busy first, then the flag: either this probe sees the reload over and
        // does nothing, or the test, after ending the reload, sees busy and waits.
        st->busy.store(true);
        if (!st->inReload.load()) {
            st->busy.store(false);
            SwitchToThread();
            continue;
        }
        HANDLE h = CreateFileW(path->c_str(), DELETE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const DWORD code = h == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        const bool wholly = st->inReload.load();  // false: the reload ended mid-probe
        st->busy.store(false);
        if (!wholly) continue;
        st->inside.fetch_add(1);
        if (code == ERROR_SHARING_VIOLATION) st->refused.fetch_add(1);
        else if (code != ERROR_SUCCESS) st->other.fetch_add(1);
    }
}

static void shareDeleteCase(const std::wstring& scratch) {
    const std::string one = "[share]\r\nk = 1\r\n";
    const std::string two = "[share]\r\nk = 2\r\n";
    if (!rewriteIni(scratch, one)) {
        fail("config share delete", "could not write the scratch ini");
        return;
    }
    Config::get().init(scratch);
    const std::wstring path = scratch + L"\\edvr.ini";

    ProbeState st;
    std::thread probe(probeMain, &path, &st);

    // At least 300 reloads, and on until the probe has landed 300 times inside
    // them: a probe thread that is starved by a busy machine (the rigs run in a
    // pool) gets the time it needs rather than failing the run for want of it.
    // The cap is what ends a probe that never runs.
    int  reloads = 0;
    int  unparsed = 0;
    bool writeFailed = false;
    const ULONGLONG t0 = GetTickCount64();
    for (int i = 0; i < 20000; ++i) {
        if (i >= 300 && st.inside.load() >= 300) break;
        if (i >= 300 && GetTickCount64() - t0 > 8000) break;
        if (!rewriteIni(scratch, (i & 1) ? one : two)) {
            writeFailed = true;
            break;
        }
        st.inReload.store(true);
        const bool parsed = Config::get().reloadIfChanged();
        st.inReload.store(false);
        while (st.busy.load()) SwitchToThread();  // a probe in flight finishes before the file is rewritten
        if (!parsed) ++unparsed;
        ++reloads;
    }
    st.stop.store(true);
    probe.join();

    const long inside = st.inside.load();
    const long refused = st.refused.load();
    if (writeFailed) {
        fail("config share delete", "could not rewrite the scratch ini");
    } else if (unparsed) {
        fail("config share delete", std::to_string(unparsed) + " of " + std::to_string(reloads) +
                                        " reloads found the file unchanged, so they never read it");
    } else if (inside < 100) {
        fail("config share delete", "the probe made only " + std::to_string(inside) +
                                        " attempts inside " + std::to_string(reloads) +
                                        " reloads; too few to say anything");
    } else if (refused > 10) {
        fail("config share delete",
             std::to_string(refused) + " of " + std::to_string(inside) +
                 " attempts to open the ini for delete were refused while a reload had it open: "
                 "Config's read does not share DELETE");
    } else {
        ok("a reload leaves edvr.ini open to being replaced (FILE_SHARE_DELETE)");
    }
    printf("  info  %ld probes inside %d reloads, %ld refused, %ld failed another way\n", inside,
           reloads, refused, st.other.load());
}

// --- the settings file's name, in every message that carries it (2026-09-30) -------------
//
// Flight 052916, on an install that read edvr-flat.ini: the F8 panel said "written to
// edvr.ini", the config audit said "edvr.ini: 1 line(s) name settings this build does not
// read" about a line that was in edvr-flat.ini, and a chained mod stayed loaded because the
// line that turned it off had been commented out in edvr.ini while edvr-flat.ini still had it
// active. Config::iniName() says which file the process opened; ini_name.h says why nothing
// else may spell the name. Held here three ways: the resolver against the four situations a
// process can be in (real Config, real files, real log), the messages that come out of them,
// and a scan of the sources for a message that spells a name.

static void removeIniFile(const std::wstring& dir, const wchar_t* leaf) {
    DeleteFileW((dir + L"\\" + leaf).c_str());
}

// A path as text for a failure line (ASCII paths; anything else prints as '?').
static std::string narrow(const std::wstring& w) {
    std::string out;
    for (wchar_t c : w) out.push_back(c < 128 ? static_cast<char>(c) : '?');
    return out;
}

// The pure resolver, for a path a caller already holds.
static void iniNamePathCases() {
    static const struct { const wchar_t* path; const char* want; const char* what; } kPaths[] = {
        {L"C:\\Games\\Elite\\edvr-flat.ini", "edvr-flat.ini", "a path to edvr-flat.ini names it"},
        {L"C:\\Games\\Elite\\edvr.ini", "edvr.ini", "a path to edvr.ini names it"},
        {L"C:/Games/Elite/EDVR-FLAT.INI", "edvr-flat.ini", "the name is compared without case, and either slash"},
        {L"edvr-flat.ini", "edvr-flat.ini", "a bare file name is a path too"},
        {L"C:\\Games\\edvr-flat.ini.bak", "edvr.ini", "a backup's name is not the file"},
        {L"C:\\Games\\my-edvr-flat.ini", "edvr.ini", "nor is a longer name that ends the same way"},
        {L"", "edvr.ini", "no path at all falls back to the VR file's name"},
    };
    for (const auto& p : kPaths) {
        const char* got = iniNameOfPath(p.path);
        if (std::strcmp(got, p.want) == 0) ok(p.what);
        else fail(p.what, std::string("\"") + got + "\", wanted \"" + p.want + "\"");
    }
}

// The directory this process's exe is in: the other place Config::init looks for a settings file.
static std::wstring exeDirectory() {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    const std::wstring path(buf, n);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// Four situations, each in a directory of its own with its own log. The file that is NOT read
// carries a different dead line and a different moved key, so a message about the wrong file
// shows in the text as well as in the name.
//
// Run only by iniNameFixturesIsolated(), in a process whose exe directory holds no ini: Config::init
// looks in the exe's directory too (a flat process reads <exe dir>\edvr-flat.ini when its module
// directory has none), and in the build that is build\, where the flat edition's staged
// edvr-flat.ini sits. The first version of these fixtures ran in this process, asked for "flat,
// no edvr-flat.ini yet" and was handed build\'s file: the flat-fallback fixture failed in the
// build and passed from a scratch directory, which is how it got past the author.
static void iniNameFixtures(const std::wstring& scratch) {
    const RuntimeProfile savedProfile = g_runtimeProfile;
    Config& cfg = Config::get();

    // The premise, asserted rather than assumed: nothing beside this exe for the loader to find.
    {
        const std::wstring exeDir = exeDirectory();
        static const wchar_t* kBeside[] = {L"edvr.ini", L"edvr-flat.ini", L"edvr_profile.ini"};
        std::string found;
        for (const wchar_t* leaf : kBeside) {
            if (GetFileAttributesW((exeDir + L"\\" + leaf).c_str()) != INVALID_FILE_ATTRIBUTES)
                found += std::string(found.empty() ? "" : ", ") + narrow(leaf);
        }
        if (exeDir.empty() || !found.empty()) {
            fail("the fixtures run beside no ini of their own",
                 "the exe's directory " + narrow(exeDir) + " holds " + (found.empty() ? "no exe path" : found));
            return;
        }
        ok("the fixtures run in a directory with no ini beside the exe");
    }

    static const char* kKnown[] = {"advanced.d3d11_fixes", "experimental.new_name"};
    static const char* kMoved[][3] = {{"fix.old_name_1", "experimental.new_name", ""},
                                      {"fix.old_name_2", "experimental.new_name", ""},
                                      {"fix.old_name_3", "experimental.new_name", ""}};
    struct Fixture {
        const wchar_t* suffix;
        const wchar_t* tag;
        const char* descriptor;
        bool writeVr, writeFlat;
        int index;                 // which dead and moved key this fixture's read file carries
        const char* wantName;
        const char* otherName;     // the name that must never appear as a message's file
        const char* what;
    };
    static const Fixture kFixtures[] = {
        {L"_ininame_vr", L"ininamevr", "[install]\r\nschema = 1\r\nprofile = vr\r\n", true, true, 1, "edvr.ini",
         "edvr-flat.ini", "VR profile, with an edvr-flat.ini lying beside it"},
        {L"_ininame_flat", L"ininameflat", "[install]\r\nschema = 1\r\nprofile = flat\r\n", true, true, 2,
         "edvr-flat.ini", "edvr.ini", "flat profile, both files present (the flight's install)"},
        {L"_ininame_fallback", L"ininamefall", "[install]\r\nschema = 1\r\nprofile = flat\r\n", true, false, 3,
         "edvr.ini", "edvr-flat.ini", "flat profile, no edvr-flat.ini yet: it reads edvr.ini whole"},
        {L"_ininame_none", L"ininamenone", "[install]\r\nschema = 1\r\nprofile = flat\r\n", false, false, 0,
         "edvr-flat.ini", "edvr.ini", "flat profile, neither file: the one it would create"},
    };

    Log::get().close();
    for (const Fixture& f : kFixtures) {
        const std::wstring dir = scratch + f.suffix;
        CreateDirectoryW(dir.c_str(), nullptr);
        removeIniFile(dir, L"edvr.ini");
        removeIniFile(dir, L"edvr-flat.ini");
        deleteLogs(dir, f.tag);
        // The read file carries: a dead line, a moved key still on its old name, a malformed
        // yes/no. The other file carries a dead line and a moved key of its own (never reported).
        const std::string mine = "[advanced]\r\nd3d11_fixes = maybe\r\n[fix]\r\nstale_line_" +
                                 std::to_string(f.index) + " = on\r\nold_name_" + std::to_string(f.index) +
                                 " = 3\r\n";
        const std::string other = "[fix]\r\nnot_the_file_read = on\r\nold_name_9 = 4\r\n";
        const bool flatRead = std::strcmp(f.wantName, "edvr-flat.ini") == 0;
        bool wrote = writeIni(dir, f.descriptor, L"edvr_profile.ini");
        if (f.writeVr) wrote = wrote && writeIni(dir, (flatRead ? other : mine).c_str(), L"edvr.ini");
        if (f.writeFlat) wrote = wrote && writeIni(dir, (flatRead ? mine : other).c_str(), L"edvr-flat.ini");
        if (!wrote) {
            fail(f.what, "could not write the fixture");
            continue;
        }
        cfg.setAuditTables(kKnown, 2, kMoved, 3);
        cfg.init(dir);
        const std::string got = cfg.iniName();
        if (got == f.wantName) ok(f.what);
        else fail(f.what, "iniName() said " + got + ", wanted " + f.wantName);
        if (!f.writeVr && !f.writeFlat) {
            // Nothing to read, nothing to say: the name is all this fixture asks.
            cfg.setAuditTables(nullptr, 0, nullptr, 0);
            continue;
        }
        if (!Log::get().open(dir, f.tag)) {
            fail(f.what, "the log would not open");
            cfg.setAuditTables(nullptr, 0, nullptr, 0);
            continue;
        }
        (void)cfg.getBool("advanced.d3d11_fixes", true);   // says the malformed yes/no, flushes the audit's queue
        Log::get().close();
        cfg.setAuditTables(nullptr, 0, nullptr, 0);
        const std::string body = readNewestLog(dir, f.tag);
        const std::string idx = std::to_string(f.index);
        const std::string mineName = f.wantName;
        struct Need { std::string needle; const char* what; };
        const Need needs[] = {
            {mineName + ": 1 line(s) name settings this build does not read: fix.stale_line_" + idx,
             "the config audit's dead-line note names the file it read"},
            {mineName + ": fix.old_name_" + idx + " has moved to experimental.new_name",
             "the moved-key note names it"},
            {mineName + ": advanced.d3d11_fixes = \"maybe\" is not a yes/no value",
             "a malformed yes/no names it"},
        };
        for (const Need& n : needs) {
            if (body.find(n.needle) != std::string::npos) ok((std::string(f.what) + ": " + n.what).c_str());
            else fail((std::string(f.what) + ": " + n.what).c_str(), "\"" + n.needle + "\" is not in the log");
        }
        // Never the other file: not at the front of a message, and nothing said about its own lines.
        // A log line starts "[hh:mm:ss.mmm] " and then the message, so look for the other name right
        // after that (edvr-flat.ini never matches "edvr.ini: ", the names differ before the dot).
        const std::string otherPrefix = std::string(f.otherName) + ": ";
        bool otherFirst = false;
        for (size_t at = body.find(otherPrefix); at != std::string::npos; at = body.find(otherPrefix, at + 1)) {
            if (at >= 2 && body[at - 1] == ' ' && body[at - 2] == ']') otherFirst = true;
        }
        if (!otherFirst) ok((std::string(f.what) + ": no message names the other file").c_str());
        else fail((std::string(f.what) + ": a message names the other file").c_str(),
                  std::string("\"") + otherPrefix + "\" starts a message");
        if (body.find("not_the_file_read") == std::string::npos && body.find("old_name_9") == std::string::npos)
            ok((std::string(f.what) + ": nothing is said about the file it did not read").c_str());
        else fail((std::string(f.what) + ": it reported the file it did not read").c_str(), "a line of the other file is in the log");
    }
    g_runtimeProfile = savedProfile;
    // The last line the parent looks for: a process that died part-way says nothing after its
    // failures, and "no failure lines" must not read as "all four ran".
    ok("the four situations ran to the end");
}

// Runs iniNameFixtures() in a copy of this exe kept in a directory that holds nothing else, and
// takes its lines as this process's own. The copy is a child whose stdout goes to a file: a few
// dozen lines, read back whole once it has exited.
static void iniNameFixturesIsolated(const std::wstring& scratchArg) {
    const char* const what = "the settings file's name in a process of its own";
    wchar_t self[MAX_PATH * 2]{};
    const DWORD selfLen = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(sizeof(self) / sizeof(self[0])));
    if (selfLen == 0 || selfLen >= sizeof(self) / sizeof(self[0])) {
        fail(what, "could not find this exe's own path");
        return;
    }
    // The child starts in a directory of its own, so it is given the scratch directory in full.
    wchar_t full[MAX_PATH * 2]{};
    const DWORD fullLen = GetFullPathNameW(scratchArg.c_str(), static_cast<DWORD>(sizeof(full) / sizeof(full[0])),
                                           full, nullptr);
    if (fullLen == 0 || fullLen >= sizeof(full) / sizeof(full[0])) {
        fail(what, "could not make " + narrow(scratchArg) + " a full path");
        return;
    }
    const std::wstring scratch(full, fullLen);
    const std::wstring home = scratch + L"_ininame_exe";
    const std::wstring exe = home + L"\\config_test.exe";
    const std::wstring outPath = home + L"\\output.txt";
    CreateDirectoryW(home.c_str(), nullptr);
    if (!CopyFileW(self, exe.c_str(), FALSE)) {
        fail(what, "could not copy the exe to " + narrow(home) + " (error " + std::to_string(GetLastError()) + ")");
        return;
    }

    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE out = CreateFileW(outPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        fail(what, "could not open " + narrow(outPath) + " (error " + std::to_string(GetLastError()) + ")");
        return;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out;
    si.hStdError = out;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\" --ininame-cases \"" + scratch + L"\"";
    const BOOL started = CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, TRUE, 0, nullptr,
                                        home.c_str(), &si, &pi);
    const DWORD startError = GetLastError();
    CloseHandle(out);
    if (!started) {
        fail(what, "could not start " + narrow(exe) + " (error " + std::to_string(startError) + ")");
        return;
    }
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 120000) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        fail(what, "the process did not finish in two minutes");
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    std::string text;
    if (FILE* f = _wfopen(outPath.c_str(), L"rb")) {
        char chunk[4096];
        size_t got;
        while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) text.append(chunk, got);
        fclose(f);
    }
    // Its lines are this run's lines; its failures are this run's failures.
    bool sawLast = false;
    bool sawFailure = false;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        printf("%s\n", line.c_str());
        if (line.compare(0, 6, "  FAIL") == 0) {
            ++g_fails;
            sawFailure = true;
        } else if (line == "  ok    the four situations ran to the end") {
            sawLast = true;
        }
    }
    if (!sawLast && !sawFailure) fail(what, "the process printed no result (exit code " + std::to_string(code) + ")");
    else if (code != 0 && !sawFailure) fail(what, "the process exited with code " + std::to_string(code));
}

static void iniNameCases(const std::wstring& scratch) {
    iniNamePathCases();
    iniNameFixturesIsolated(scratch);
}

// A message that spells the settings file's name is the fault; tools\config_test knows it by scanning.
// String literals only -- a comment may name a file -- lexed, not grepped, so a quote inside a char
// literal, an apostrophe in a comment or a raw string cannot hide one or invent one.
static std::vector<std::string> namedLiterals(const std::string& text) {
    std::vector<std::string> hits;
    const size_t n = text.size();
    size_t line = 1;
    auto lower = [](std::string s) { for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c))); return s; };
    auto check = [&](const std::string& literal, size_t atLine) {
        const std::string l = lower(literal);
        if (l.find("edvr.ini") != std::string::npos || l.find("edvr-flat.ini") != std::string::npos)
            hits.push_back("line " + std::to_string(atLine) + ": \"" + literal.substr(0, 70) + "\"");
    };
    for (size_t i = 0; i < n;) {
        const char c = text[i];
        if (c == '\n') { ++line; ++i; continue; }
        if (c == '/' && i + 1 < n && text[i + 1] == '/') {          // line comment
            while (i < n && text[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && text[i + 1] == '*') {          // block comment
            i += 2;
            while (i + 1 < n && !(text[i] == '*' && text[i + 1] == '/')) { if (text[i] == '\n') ++line; ++i; }
            i += 2;
            continue;
        }
        if (c == '\'') {                                            // char literal (or a digit separator)
            // A separator sits inside a number (0x1'0000): the token it is in starts with a digit.
            // L'a' and u8'a' are char literals: their token starts with a letter.
            size_t s = i;
            while (s > 0 && (isalnum(static_cast<unsigned char>(text[s - 1])) || text[s - 1] == '_' ||
                             text[s - 1] == '\'' || text[s - 1] == '.'))
                --s;
            const bool separator = s < i && isdigit(static_cast<unsigned char>(text[s])) && i + 1 < n &&
                                   isalnum(static_cast<unsigned char>(text[i + 1]));
            ++i;
            if (separator) continue;
            while (i < n && text[i] != '\'' && text[i] != '\n') { if (text[i] == '\\') ++i; ++i; }
            ++i;
            continue;
        }
        if (c == '"') {
            const size_t atLine = line;
            const bool raw = i > 0 && text[i - 1] == 'R';
            std::string literal;
            if (raw) {                                              // R"delim( ... )delim"
                size_t d = i + 1;
                std::string delim;
                while (d < n && text[d] != '(' && text[d] != '\n' && delim.size() < 17) delim += text[d++];
                const std::string close = ")" + delim + "\"";
                const size_t end = text.find(close, d);
                const size_t stop = end == std::string::npos ? n : end;
                literal = text.substr(d + 1 < n ? d + 1 : n, stop > d ? stop - d - 1 : 0);
                for (char ch : literal) if (ch == '\n') ++line;
                i = end == std::string::npos ? n : end + close.size();
            } else {
                ++i;
                while (i < n && text[i] != '"' && text[i] != '\n') {
                    if (text[i] == '\\' && i + 1 < n) { literal += text[i + 1]; i += 2; continue; }
                    literal += text[i++];
                }
                ++i;
            }
            check(literal, atLine);
            continue;
        }
        ++i;
    }
    return hits;
}

// Every source under src\ but the installer's (which manages both files and says which it means); withInstaller reads those too.
static void collectSources(const std::wstring& dir, const std::wstring& relative,
                           std::vector<std::wstring>* out, bool withInstaller = false) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring rel = relative.empty() ? name : relative + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!withInstaller && _wcsicmp(rel.c_str(), L"installer") == 0) continue;
            collectSources(dir + L"\\" + name, rel, out, withInstaller);
            continue;
        }
        const size_t dot = name.find_last_of(L'.');
        const std::wstring ext = dot == std::wstring::npos ? L"" : name.substr(dot);
        if (_wcsicmp(ext.c_str(), L".cpp") == 0 || _wcsicmp(ext.c_str(), L".h") == 0 ||
            _wcsicmp(ext.c_str(), L".inc") == 0 || _wcsicmp(ext.c_str(), L".hpp") == 0)
            out->push_back(rel);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void iniNameScan(const std::wstring& root) {
    // The scanner itself first: it must find what it is for, and only that.
    const std::string violating =
        "void f() {\n"
        "    Log::get().note(\"edvr.ini: advanced.x is bad\");\n"      // a message: the fault
        "    // \"edvr.ini\" in a comment is fine\n"
        "    /* and \"edvr-flat.ini\" in a block comment */\n"
        "    const char q = '\"'; const char* s = \"a quote \\\" and edvr-flat.ini\";\n"   // the literal ends at its own quote
        "    const wchar_t* w = L\"\\\\EDVR.INI\";\n"                   // wide, and without case
        "    const char* r = R\"(raw edvr.ini text)\";\n"
        "    const char* clean = \"edvr_profile.ini and edvr_openxr.ini are other files\";\n"
        "}\n";
    const std::vector<std::string> control = namedLiterals(violating);
    if (control.size() == 4 && control[0].rfind("line 2:", 0) == 0 && control[1].rfind("line 5:", 0) == 0 &&
        control[2].rfind("line 6:", 0) == 0 && control[3].rfind("line 7:", 0) == 0)
        ok("the source scan finds a message, a wide literal and a raw string that spell the file, and skips comments, char literals and other ini files");
    else fail("the source scan's own control", std::to_string(control.size()) + " hits, not the four literals at lines 2, 5, 6 and 7");

    std::vector<std::wstring> files;
    collectSources(root + L"\\src", L"", &files);
    if (files.size() < 150) {
        fail("source scan", "found only " + std::to_string(files.size()) + " sources under " +
                                narrow(root) + "\\src; the scan would prove nothing");
        return;
    }
    // The one place the names are spelled. The installer is not scanned: it manages both files.
    int scanned = 0, bad = 0;
    for (const std::wstring& rel : files) {
        if (_wcsicmp(rel.c_str(), L"common\\ini_name.h") == 0) continue;
        const std::string text = readRepoFile(root, (L"src\\" + rel).c_str());
        ++scanned;
        for (const std::string& hit : namedLiterals(text)) {
            ++bad;
            fail("a message spells the settings file's name; use Config::iniName()",
                 narrow(rel) + " " + hit);
        }
    }
    if (!bad) {
        ok(("no string literal in " + std::to_string(scanned) +
            " sources under src (the installer aside) spells edvr.ini or edvr-flat.ini; ini_name.h is the one place")
               .c_str());
    }
}

// The three experimental keys retired on 2026-10-01 chose behaviour that is now permanent: the VR world route's jitter (always on
// while the route owns the world; experimental.temporal_aa_jitter off still stops it), the depth-validated steady detail (always on,
// in the VR world route and in the flat runtime) and the switch that made the jitter phases follow the upscale ratio (they always
// do). The fifteen keys of the old Explorer Cam route went on 2026-10-07 (docs/design-explorer-cam-free-camera-2026-10-07.md): the
// press-counting gate, its camera-key adoption and the headset-pose offset, replaced by Explorer Cam's redesign. Nothing may still spell
// their names. A reader left behind would let a user's old line act, a log message would send people
// to a setting that does nothing, and the shipped file would document one. The installer carries an old line over under "this
// version no longer uses it" (installer_test holds that), so only the two tests spell the names.
//
// The four keys of the settlement LOD governor and the static prop gate, removed 2026-10-08, are held out the same way, but for a
// different reason: the behaviour is gone with them (the game's own detail and its own prop updates are what run), so there is
// nothing to keep permanent. "settlement_detail" is also a prefix of the governor's two [advanced] keys, settlement_detail_max
// and settlement_detail_observe, so the one name holds all three.
//
// advanced.explorer_cam_probe, the temporary log-only instruments of the Explorer Cam redesign (the free-camera observer, the camera census's
// implicit tally, the scene-block fingerprint and the later controller, neck, fade and skeleton instruments behind the same key), was removed
// 2026-10-09 when the arc's flights were done. No live key contains its name, so the bare name holds. An old line in a user's ini is carried over by
// the installer under "no longer used by this version" and is never read by the DLL.
//
// The six keys of the terrain guard, removed 2026-10-09 (fix.cull_guard, its _percent, _fraction_h, _fraction_v and _headsets, and advanced.cull_guard_channel),
// are held out the same way, the one bare name covering all six. The cause the guard worked around was found: Elite culled with a head pose 42 ms older than
// the one it drew with, and EDVR now answers its "now" request at the drawn frame's display time (docs/terrain-culling.md). Nothing is left to keep permanent.
static const char* const kRetiredKeys[] = {
    "temporal_aa_on_foot_world_jitter",
    "temporal_aa_on_foot_world_steady_detail",
    "temporal_aa_jitter_follows_upscale",
    "settlement_detail",
    "static_prop_updates",
    // The old Explorer Cam route, 2026-10-07. head_offset_view also covers head_offset_view_count.
    "head_offset_gate",
    "head_offset_view",
    "head_offset_intent_grace_ms",
    "head_offset_enter_window_ms",
    "head_offset_right",
    "head_offset_up",
    "head_offset_forward",
    "head_yaw_degrees",
    "head_offset_external_only",
    "head_offset_game_poses",
    "head_offset_max_stale_frames",
    "dump_camera_on_external_cam",
    "keyless_camera",
    "hold_frames_on_external_cam",
    // The Explorer Cam redesign's temporary probe, 2026-10-09.
    "explorer_cam_probe",
    // The advanced and experimental key cull, 2026-10 (the installer carries an old line over; the DLL never reads it). The two names
    // that begin a live key (temporal_aa_jitter_phases, the on-foot world route's retired siblings) are in kRetiredExactKeys.
    "temporal_aa_motion", "temporal_aa_current", "temporal_aa_history_sharp", "temporal_aa_jitter_sign",
    "temporal_aa_jitter_lag", "temporal_aa_ship_metres", "temporal_aa_debug", "temporal_aa_diagnostics",
    "temporal_aa_fsr_reactive", "temporal_aa_fsr_debug", "temporal_aa_warm", "temporal_aa_smoke_floor",
    "temporal_aa_smoke_reactive", "temporal_aa_menu_metres", "temporal_aa_movers_tolerance", "temporal_aa_movers_strength",
    "temporal_aa_hologram_families", "temporal_aa_hologram_floor", "temporal_aa_hologram_share", "temporal_aa_hologram_depth",
    "eye_run_paired", "eye_run_treated", "ui_depth_reactive", "ui_ghost_tolerance", "corona_smear_level", "temporal_aa_fovea",
    "temporal_aa_periphery", "temporal_aa_partial", "temporal_aa_movers", "temporal_aa_blend", "temporal_aa_clamp",
    "temporal_aa_before_post", "on_foot_maps_sharp", "sun_glare_variant", "sun_glare_probe",
    "target_indicator_vs", "target_indicator_scale_probe", "wake_pulse_indices", "fss_eye_dump", "fss_eye_series",
    "particle_probe", "intro_probe", "openvr_census", "ui_depth_eyes", "ui_depth_test", "dispatch_pair_sync",
    "dispatch_cb1_lend", "dispatch_cb1_strip", "ui_depth_menus", "ui_depth_variants", "ui_depth_alpha", "ui_depth_planes",
    "ui_depth_families", "ui_depth_exclude", "menu_backdrop_threshold", "menu_backdrop_dither", "intro_video_distance",
    "intro_video_deband", "intro_video_dither", "intro_video_sharpen", "holo_pattern_level", "loading_dim_level",
    "target_indicator_sharpen", "flat_cb_map_cache", "flat_context_isolation",
    // The terrain guard, 2026-10-09: all six keys begin with this.
    "cull_guard",
};
static const int kRetiredKeyCount = int(sizeof(kRetiredKeys) / sizeof(kRetiredKeys[0]));

// Retired by EXACT name, because the bare name is a substring of live keys. [fix] explorer_cam, the on/off switch of the redesigned Explorer Cam, was removed
// 2026-10-08: Explorer Cam is armed exactly when hotkey.explorer_cam is set. Its qualified name "fix.explorer_cam" is also the beginning of every live
// fix.explorer_cam_* key (_eye_up, _eye_trim_right, _follow_smoothing_ms ...), so a match counts only when the next character is not part of a name. A hotkey
// line spells "hotkey.explorer_cam", which never contains "fix.". The shipped file's [fix] section is held out by the key lookup in main (expectStr, "<unset>"),
// because there the key is the bare "explorer_cam", which sits in [hotkey] too.
static const char* const kRetiredExactKeys[] = {
    "fix.explorer_cam",
    // The 2026-10 cull's two names that begin a live or already-retired key: the global jitter switch (temporal_aa_jitter_phases stays)
    // and the world route's switch (its _jitter and _steady_detail siblings went on 2026-10-01).
    "temporal_aa_jitter",
    "temporal_aa_on_foot_world",
};
static const int kRetiredExactKeyCount = int(sizeof(kRetiredExactKeys) / sizeof(kRetiredExactKeys[0]));

// True when `text` spells `name` followed by something that is not part of a key name.
static bool spellsExactKey(const std::string& text, const char* name) {
    const size_t n = std::strlen(name);
    for (size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + 1)) {
        const char next = at + n < text.size() ? text[at + n] : '\0';
        if (!(std::isalnum(static_cast<unsigned char>(next)) || next == '_')) return true;
    }
    return false;
}

// The first retired name `text` spells, or nullptr.
static const char* spellsRetiredKey(const std::string& text) {
    for (const char* name : kRetiredKeys)
        if (text.find(name) != std::string::npos) return name;
    for (const char* name : kRetiredExactKeys)
        if (spellsExactKey(text, name)) return name;
    return nullptr;
}

static void retiredKeyScan(const std::wstring& root) {
    // The scan itself first: it must find each name wherever it sits, and pass the live keys that sit beside them.
    int found = 0;
    for (const char* name : kRetiredKeys) {
        const std::string read = std::string("Config::get().getString(\"experimental.") + name + "\", \"on\")";
        const std::string logged = std::string("the log says experimental.") + name + " has nothing to act on";
        const std::string commented = std::string("# experimental.") + name + " = on\n";
        const char* a = spellsRetiredKey(read);
        const char* b = spellsRetiredKey(logged);
        const char* c = spellsRetiredKey(commented);
        if (a && b && c && std::strcmp(a, name) == 0 && std::strcmp(b, name) == 0 && std::strcmp(c, name) == 0) ++found;
    }
    const std::string live =
        "advanced.temporal_aa_jitter_phases = 8\n"           // the live flat jitter cycle: a retired name starts it
        "advanced.texture_lod_bias = 0\n"
        "advanced.vr_camera_census = 0\n";   // (cull_guard_channel stood here while it was live; main retired it)
    if (found == kRetiredKeyCount && !spellsRetiredKey(live))
        ok(("the retired-key scan finds each of the " + std::to_string(kRetiredKeyCount) +
            " names in a read, a message and a comment, and passes the live keys beside them").c_str());
    else
        fail("the retired-key scan's own control", std::to_string(found) + " of " + std::to_string(kRetiredKeyCount) +
                                                       " names found, or a live key was flagged");

    // The exact-name scan's own control: it finds the retired switch in a read, a message, a comment and at the end of a sentence or of the text, and passes
    // every live key that begins with it or shares its tail (the hotkey, the eye keys, the trims, the smoothing).
    {
        const std::string retired[] = {
            "Config::get().getBool(\"fix.explorer_cam\", true)",
            "the log says set fix.explorer_cam to off",
            "# fix.explorer_cam = on\n",
            "turns it off (fix.explorer_cam).",
            "fix.explorer_cam",
        };
        const std::string liveKeys[] = {
            "Config::get().getString(\"hotkey.explorer_cam\", \"F5\")",
            "[hotkey]\nexplorer_cam = F5\n",
            "hotkey.explorer_cam)",
            "cfg.getFloat(\"fix.explorer_cam_eye_up\", 1.68f)",
            "fix.explorer_cam_eye_forward fix.explorer_cam_eye_right",
            "fix.explorer_cam_eye_trim_right, _up, _forward",
            "fix.explorer_cam_follow_smoothing_ms",
        };
        bool foundAll = true, passedAll = true;
        for (const std::string& t : retired) foundAll = foundAll && spellsRetiredKey(t) != nullptr && std::strcmp(spellsRetiredKey(t), kRetiredExactKeys[0]) == 0;
        for (const std::string& t : liveKeys) passedAll = passedAll && spellsRetiredKey(t) == nullptr;
        if (foundAll && passedAll && kRetiredExactKeyCount == 3)
            ok("the exact-name scan finds fix.explorer_cam in a read, a message, a comment and at the end of a sentence, and passes hotkey.explorer_cam, "
               "fix.explorer_cam_eye_*, the trims and the smoothing beside it");
        else
            fail("the exact-name scan's own control", std::string(foundAll ? "" : "a retired spelling was missed; ") + (passedAll ? "" : "a live key was flagged"));
    }

    // The shipped file: no template, no comment, no mention.
    const std::string ini = readRepoFile(root, L"edvr.ini");
    if (ini.empty()) {
        fail("edvr.ini is readable from the repo root (retired keys)", "could not read it");
    } else if (const char* name = spellsRetiredKey(ini)) {
        fail("the shipped edvr.ini names none of the retired keys", std::string("it spells ") + name);
    } else {
        ok("the shipped edvr.ini defines, documents and mentions none of the retired keys");
    }

    // Every production source, the installer's included: no reader, no allow-list entry, no log line.
    std::vector<std::wstring> files;
    collectSources(root + L"\\src", L"", &files, true);
    if (files.size() < 150) {
        fail("retired-key scan", "found only " + std::to_string(files.size()) + " sources under " + narrow(root) +
                                     "\\src; the scan would prove nothing");
        return;
    }
    int scanned = 0, bad = 0;
    for (const std::wstring& rel : files) {
        const std::string text = readRepoFile(root, (L"src\\" + rel).c_str());
        ++scanned;
        if (const char* name = spellsRetiredKey(text)) {
            ++bad;
            fail("a production source spells a retired key", narrow(rel) + " spells " + name);
        }
    }
    if (!bad) {
        ok(("none of the " + std::to_string(scanned) +
            " sources under src (the installer's included) spells a retired key: no reader, no allow-list entry, no message")
               .c_str());
    }
}

int main(int argc, char** argv) {
    // Unbuffered, so a crash does not take the output with it: the first run of
    // this test appeared to die before its first printf, which was only the
    // buffer being discarded.
    setvbuf(stdout, nullptr, _IONBF, 0);

    // The child iniNameFixturesIsolated() starts from a directory of its own: only the resolver's
    // four real-file situations, no shipped ini read, and an exit code that says whether all held.
    if (argc == 3 && std::strcmp(argv[1], "--ininame-cases") == 0) {
        iniNameFixtures(widen(argv[2]));
        return g_fails == 0 ? 0 : 1;
    }

    if (argc < 2) {
        printf("usage: config_test.exe <dir containing edvr.ini> [scratch dir]\n");
        return 2;
    }
    const std::wstring dir = widen(argv[1]);

    Config::get().init(dir);
    if (Config::get().path().empty()) {
        printf("  FAIL  no edvr.ini found under %s\n", argv[1]);
        return 1;
    }
    printf("  read  %S\n", Config::get().path().c_str());

    // --- the shipped file, as the parser sees it ---------------------------
    //
    // The opening [fix] and [hotkey] blocks.
    expectStr("fix.temporal_aa_model", "k", "DLSS preset ships as K on the Performance page");
    expectStr("experimental.supersample_resolve", "<unset>",
              "the retired supersample_resolve key is still absent");
    expectStr("experimental.supersample_filter", "<unset>",
              "the retired supersample_filter key is still absent");
    expectBool("fix.night_vision_stability", true, "night vision pulse stability remains a standard enabled fix");
    expectBool("experimental.night_vision_realistic", false, "Realistic nightvision ships disabled under Experimental");
    expectFloat("experimental.night_vision_brightness", 8.0f, "night vision appearance brightness lives under Experimental");
    expectStr("fix.ui_depth", "<unset>", "UI depth is bundled with temporal AA");
    expectStr("fix.temporal_aa_objects", "<unset>", "station motion is bundled with temporal AA");
    expectStr("fix.temporal_aa_smoke", "<unset>", "smoke depth is bundled with temporal AA");
    expectStr("fix.engine_motion", "<unset>", "engine motion is bundled with temporal AA: its own key is retired");
    // The estimation paths the engine records superseded (2026-09-23): their
    // keys are retired, not merely off.
    expectStr("advanced.temporal_aa_estimated_objects", "<unset>", "the retired estimated station/ship paths' key is absent");
    expectStr("advanced.mesh_motion", "<unset>", "the retired mesh record pairing's key is absent");
    expectStr("advanced.temporal_aa_objects_reach", "<unset>", "the retired station path's reach is absent");
    expectStr("advanced.temporal_aa_objects_ships_metres", "<unset>", "the retired ship path's range is absent");
    // The terrain patches' own recorded transforms (2026-10-01): terrain takes the camera's motion, with no key.
    expectStr("advanced.terrain_motion", "<unset>", "the retired terrain motion key is absent");
    // The three experimental switches retired 2026-10-01: the world route's jitter A/B, its depth-checked steady detail (also the flat
    // runtime's) and the jitter-phase count's follow-the-upscale switch. The behaviour each chose is permanent, with no key; the file's
    // text and every source are held clear of the names by retiredKeyScan, and these three read the file as the parser sees it.
    expectStr("experimental.temporal_aa_on_foot_world_jitter", "<unset>", "the retired world jitter key is absent");
    expectStr("experimental.temporal_aa_on_foot_world_steady_detail", "<unset>", "the retired steady-detail key is absent");
    expectStr("experimental.temporal_aa_jitter_follows_upscale", "<unset>", "the retired jitter-phase switch is absent");
    // The 2026-10 cull: every removed advanced and experimental key reads as unset in the shipped file, under either section.
    {
        static const char* const culled[] = {
            "temporal_aa_motion",
            "temporal_aa_current",
            "temporal_aa_history_sharp",
            "temporal_aa_jitter_sign",
            "temporal_aa_jitter_lag",
            "temporal_aa_ship_metres",
            "temporal_aa_debug",
            "temporal_aa_diagnostics",
            "temporal_aa_fsr_reactive",
            "temporal_aa_fsr_debug",
            "temporal_aa_warm",
            "temporal_aa_smoke_floor",
            "temporal_aa_smoke_reactive",
            "temporal_aa_menu_metres",
            "temporal_aa_movers_tolerance",
            "temporal_aa_movers_strength",
            "temporal_aa_hologram_families",
            "temporal_aa_hologram_floor",
            "temporal_aa_hologram_share",
            "temporal_aa_hologram_depth",
            "eye_run_paired",
            "eye_run_treated",
            "ui_depth_reactive",
            "ui_ghost_tolerance",
            "corona_smear_level",
            "temporal_aa_fovea",
            "temporal_aa_periphery",
            "temporal_aa_jitter",
            "temporal_aa_partial",
            "temporal_aa_movers",
            "temporal_aa_blend",
            "temporal_aa_clamp",
            "temporal_aa_on_foot_world",
            "temporal_aa_before_post",
            "on_foot_maps_sharp",
            "sun_glare_variant",
            "sun_glare_probe",
            "target_indicator_vs",
            "target_indicator_scale_probe",
            "wake_pulse_indices",
            "fss_eye_dump",
            "fss_eye_series",
            "particle_probe",
            "intro_probe",
            "openvr_census",
            "ui_depth_eyes",
            "ui_depth_test",
            "dispatch_pair_sync",
            "dispatch_cb1_lend",
            "dispatch_cb1_strip",
            "ui_depth_menus",
            "ui_depth_variants",
            "ui_depth_alpha",
            "ui_depth_planes",
            "ui_depth_families",
            "ui_depth_exclude",
            "menu_backdrop_threshold",
            "menu_backdrop_dither",
            "intro_video_distance",
            "intro_video_deband",
            "intro_video_dither",
            "intro_video_sharpen",
            "holo_pattern_level",
            "loading_dim_level",
            "target_indicator_sharpen",
            "flat_cb_map_cache",
            "flat_context_isolation",
        };
        int present = 0;
        for (const char* name : culled) {
            for (const char* section : {"advanced.", "experimental."}) {
                if (Config::get().getString((std::string(section) + name).c_str(), "<unset>") != "<unset>") {
                    ++present;
                    fail("a removed key is absent from the shipped edvr.ini", (std::string(section) + name).c_str());
                }
            }
        }
        if (!present) ok("none of the 2026-10 cull's removed keys is defined in the shipped edvr.ini");
    }
    // The particle facing measurement (dead since 2026-08-23) retired 2026-09-23.
    expectStr("advanced.particle_face_emitter", "<unset>", "the retired particle facing key is absent");
    // The foveation's eye-tracked centre went with its gaze source (2026-09-23)
    // and its key retired 2026-09-24; the whole shading-rate feature, keys
    // and all, was removed 2026-09-29.
    expectStr("experimental.foveation_centre", "<unset>", "the retired foveation centre key is absent");
    expectBool("fix.share_exposure", true, "a key in the first [fix] reads");
    expectBool("fix.transition_flash", true, "...and another beside it");
    expectStr("hotkey.toggle_exposure", "SCROLLLOCK",
              "a key in the first [hotkey] reads");
    // The eye-run depth instrument ships as a commented template like the
    // other developer instruments: the compiled default is what a user gets.
    if (Config::get().getBool("advanced.eye_depth_capture", false) == false &&
        Config::get().getBool("advanced.eye_depth_capture", true) == true)
        ok("eye depth capture is documented but not live under [advanced]");
    else
        fail("eye depth capture is documented but not live under [advanced]",
             "the shipped file defines it live");
    // The settlement LOD governor and the static prop gate were removed 2026-10-08: their four keys are gone, not merely off. The
    // file's text and every source are held clear of the names by retiredKeyScan, and these read the file as the parser sees it.
    expectStr("fix.settlement_detail", "<unset>", "the removed settlement detail key is absent");
    expectStr("advanced.settlement_detail_max", "<unset>", "...and so is its ceiling");
    expectStr("advanced.settlement_detail_observe", "<unset>", "...and its observe-only switch");
    expectStr("fix.static_prop_updates", "<unset>", "the removed static prop gate's key is absent");
    // The terrain guard was removed 2026-10-09: its six keys are gone, not merely off.
    expectStr("fix.cull_guard", "<unset>", "the removed terrain guard key is absent");
    expectStr("fix.cull_guard_percent", "<unset>", "...and so is its percent margin");
    expectStr("fix.cull_guard_fraction_h", "<unset>", "...its horizontal fraction");
    expectStr("fix.cull_guard_fraction_v", "<unset>", "...its vertical fraction");
    expectStr("fix.cull_guard_headsets", "<unset>", "...its headset list");
    expectStr("advanced.cull_guard_channel", "<unset>", "...and its channel probe");
    // ui_quality (docs/ui-layer-2026-09-23.md) is one key for both halves:
    // the interface panels made at the target's size, and the game's
    // post-tonemap UI drawn into a per-eye layer after the upscale. Values
    // off | 100 | 125 (percent of HMD Quality 1.0; ui_layer_math.h parses
    // them, and the first spellings 1.0 / 1.25, for one release). Ships live
    // in [fix]; the default is 100 since 2026-09-29 (the interface as at HMD
    // Quality 1.0), off before that.
    // fix.hud_quality, the surfaces' own key for one day, is gone: absorbed.
    expectStr("fix.ui_quality", "100",
              "ui quality ships live in [fix] and defaults to 100");
    expectStr("fix.hud_quality", "<unset>",
              "...and the separate HUD key it absorbed is gone");
    // A file with no such line -- a hand-copied DLL over an old ini, a deleted
    // line -- gets the code's fallback, and that has to be the shipped default or
    // those installs run something the file never said. The read is in
    // ui_layer.cpp; the rig takes the literal from the call itself.
    {
        const std::string shippedUiQuality = Config::get().getString("fix.ui_quality", "<unset>");
        const std::string uiLayerSource = readRepoFile(dir, L"src\\d3d11\\ui_layer.cpp");
        if (uiLayerSource.empty()) {
            fail("ui_layer.cpp is readable from the repo root", "could not read it");
        } else {
            const std::string fallback = codeFallbackOf(uiLayerSource, "fix.ui_quality");
            if (fallback == shippedUiQuality) {
                ok("the code's fallback for fix.ui_quality is the shipped default");
            } else {
                fail("the code's fallback for fix.ui_quality is the shipped default",
                     "ui_layer.cpp falls back to \"" + fallback + "\", the ini ships \"" +
                         shippedUiQuality + "\"");
            }
            // CONTROL: the same source with the fallback put back to off.
            std::string reverted = uiLayerSource;
            const std::string from = "getString(\"fix.ui_quality\", \"" + fallback + "\")";
            const size_t at = reverted.find(from);
            if (at != std::string::npos) reverted.replace(at, from.size(), "getString(\"fix.ui_quality\", \"off\")");
            if (at != std::string::npos && codeFallbackOf(reverted, "fix.ui_quality") != shippedUiQuality) {
                ok("control: a fallback put back to off is caught");
            } else {
                fail("control: a fallback put back to off is caught",
                     at == std::string::npos ? "the call was not found to alter"
                                             : "the reverted source still matched the ini");
            }
        }
    }
    // The HDR route, the VR on-foot world route and the on-foot maps gate lost their keys in the 2026-10 cull: each ships as its
    // former default (auto, auto, on) with no setting, so there is no fallback to hold to a shipped value. retiredKeyScan below holds
    // the names out of the shipped file and the sources, and the absence checks above read the file as the parser sees it.

    // The depth-validated steady detail has no key any more either (retired 2026-10-01): both routes, the VR world route and the
    // flat runtime on foot, always ask the resolver for it (vr_world_route_test and flat_temporal_test pin both call sites), and
    // retiredKeyScan below holds the old key's name out of the shipped file and the sources.

    // The VR camera census (design doc section 82, advanced.vr_camera_census) installs a game hook when it is on, so it
    // is OFF by default in both places that can say so: the shipped file and the code's fallback for an ini that predates
    // the key. The same pair of checks as the world route's, with the same control.
    expectStr("advanced.vr_camera_census", "off", "the shipped edvr.ini ships the VR camera census off");
    {
        const std::string shippedCensus = Config::get().getString("advanced.vr_camera_census", "<unset>");
        const std::string censusSource = readRepoFile(dir, L"src\\d3d11\\vr_camera_census.cpp");
        if (censusSource.empty()) {
            fail("vr_camera_census.cpp is readable from the repo root", "could not read it");
        } else {
            const std::string fallback = codeFallbackOf(censusSource, "advanced.vr_camera_census");
            if (fallback == shippedCensus) {
                ok("the code's fallback for advanced.vr_camera_census is the shipped default");
            } else {
                fail("the code's fallback for advanced.vr_camera_census is the shipped default",
                     "vr_camera_census.cpp falls back to \"" + fallback + "\", the ini ships \"" + shippedCensus + "\"");
            }
            // CONTROL: the same source with the fallback turned to on (a census that installed its hook on every install).
            std::string flipped = censusSource;
            const std::string from = "getString(\"advanced.vr_camera_census\", \"" + fallback + "\")";
            const size_t at = flipped.find(from);
            if (at != std::string::npos)
                flipped.replace(at, from.size(), "getString(\"advanced.vr_camera_census\", \"on\")");
            if (at != std::string::npos && codeFallbackOf(flipped, "advanced.vr_camera_census") != shippedCensus) {
                ok("control: the VR camera census's fallback turned to on is caught");
            } else {
                fail("control: the VR camera census's fallback turned to on is caught",
                     at == std::string::npos ? "the call was not found to alter"
                                             : "the flipped source still matched the ini");
            }
        }
    }

    // The Explorer Cam switch reads from the shipped file, and the keys of the
    // old route it replaced are not defined there (kRetiredKeys, above).
    expectStr("fix.explorer_cam", "<unset>", "the retired [fix] explorer_cam switch is absent from the shipped file (Explorer Cam is armed by its hotkey alone)");
    expectStr("hotkey.explorer_cam", "F5", "...and the hotkey that arms Explorer Cam ships as F5");
    expectFloat("fix.explorer_cam_eye_up", 1.68f, "...the fallback eye keys ship at 1.68 up,");
    expectFloat("fix.explorer_cam_eye_forward", 0.10f, "...0.10 forward,");
    expectFloat("fix.explorer_cam_eye_right", 0.0f, "...and 0.0 right");
    // The trims and the smoothing are PERMANENT user settings (Sean, 2026-10-08) and ship with his own tuning. Three places must say the same three numbers: the
    // shipped file, the constants in explorer_cam_follow_core.h, and the code's fallback for an ini that lacks the keys (which must USE those constants).
    expectFloat("fix.explorer_cam_eye_trim_up", 0.15f, "the Explorer Cam eye trims ship at Sean's tuning: up 0.15,");
    expectFloat("fix.explorer_cam_eye_trim_forward", -0.08f, "...forward -0.08,");
    expectFloat("fix.explorer_cam_eye_trim_right", 0.0f, "...right 0.0,");
    expectFloat("fix.explorer_cam_follow_smoothing_ms", 0.0f, "...and the follow smoothing at 0 (exact follow)");
    expectStr("advanced.explorer_cam_probe", "<unset>", "the retired Explorer Cam probe key is absent from the shipped file (removed 2026-10-09; kRetiredKeys holds its name out of every source)");
    expectFloat("fix.explorer_cam_eye_trim_up", ecm::kTrimUpDefault, "the shipped up trim is the code constant ecm::kTrimUpDefault");
    expectFloat("fix.explorer_cam_eye_trim_forward", ecm::kTrimForwardDefault, "...the shipped forward trim is ecm::kTrimForwardDefault");
    expectFloat("fix.explorer_cam_eye_trim_right", ecm::kTrimRightDefault, "...the shipped right trim is ecm::kTrimRightDefault");
    expectFloat("fix.explorer_cam_follow_smoothing_ms", static_cast<float>(ecm::kSmoothingMsDefault), "...the shipped smoothing is ecm::kSmoothingMsDefault");
    {
        const float trims[3] = {ecm::kTrimUpDefault, ecm::kTrimForwardDefault, ecm::kTrimRightDefault};
        bool inside = true;
        for (float v : trims) inside = inside && v >= -ecm::kTrimLimit && v <= ecm::kTrimLimit && ecm::clampTrim(v) == v;
        if (inside && static_cast<float>(ecm::kSmoothingMsDefault) >= 0.0f && static_cast<float>(ecm::kSmoothingMsDefault) <= ecm::kSmoothingMsMax)
            ok("every shipped Explorer Cam default is inside its clamp (+-0.5 m, 0..1000 ms)");
        else
            fail("every shipped Explorer Cam default is inside its clamp", "a default sits outside what the code would let through");
    }
    {
        // The code's fallback must be the constant, not a number that happens to match today. The control rewrites each call to a literal that
        // differs and must be caught. The reads are in the settings cache (explorer_cam_settings_core.h), which explorerCamFrameBoundary reads them through once per
        // configuration change; explorer_cam_fade_test pins that the frame boundary reads none of these keys itself.
        const std::string source = readRepoFile(dir, L"src\\d3d11\\explorer_cam_settings_core.h");
        struct Read { const char* key; const char* call; const char* expr; };
        const Read reads[] = {
            {"fix.explorer_cam_eye_trim_up", "getFloat", "kTrimUpDefault"},
            {"fix.explorer_cam_eye_trim_forward", "getFloat", "kTrimForwardDefault"},
            {"fix.explorer_cam_eye_trim_right", "getFloat", "kTrimRightDefault"},
            {"fix.explorer_cam_follow_smoothing_ms", "getInt", "kSmoothingMsDefault"},
        };
        auto usesConstant = [](const std::string& text, const Read& r) {
            return text.find(std::string(r.call) + "(\"" + r.key + "\", " + r.expr + ")") != std::string::npos;
        };
        if (source.empty()) {
            fail("explorer_cam_settings_core.h is readable from the repo root", "could not read it");
        } else {
            bool all = true, controlsCaught = true;
            for (const Read& r : reads) {
                all = all && usesConstant(source, r);
                std::string flipped = source;
                const std::string from = std::string(r.call) + "(\"" + r.key + "\", " + r.expr + ")";
                const size_t at = flipped.find(from);
                if (at != std::string::npos) flipped.replace(at, from.size(), std::string(r.call) + "(\"" + r.key + "\", 0.0f)");
                controlsCaught = controlsCaught && at != std::string::npos && !usesConstant(flipped, r);
            }
            if (all) ok("explorer_cam_settings_core.h reads each trim and the smoothing with the code constant as its fallback (the shipped value, not a copy of it)");
            else fail("explorer_cam_settings_core.h reads each trim and the smoothing with the code constant as its fallback", "a read uses another fallback");
            if (controlsCaught) ok("control: a read rewritten to a literal fallback is caught");
            else fail("control: a read rewritten to a literal fallback is caught", "the rewritten source still passed, or the call was not found");
        }
    }
    expectBool("hotkey.read_game_bindings", true,
               "a key under a repeated [hotkey] reads");

    // THE CAMERA KEYS MUST STAY GONE (0.7.1). They were removed because a
    // hand-kept copy of the game's own key configuration is a second copy
    // that drifts, and this one did -- for weeks, silently, agreeing with
    // Elite's ship camera binding while disagreeing with its on-foot one.
    // Documenting them again would resurrect that: the ini value would be
    // read as an override of the bindings file, by a build that no longer
    // reads it, so the setting would appear to work and do nothing.
    expectStr("hotkey.external_camera", "<unset>",
              "the retired external_camera key is still absent");
    expectStr("hotkey.external_camera_next", "<unset>",
              "...and external_camera_next");

    // --- regressions ------------------------------------------------------
    if (argc >= 3) {
        const std::wstring scratch = widen(argv[2]);
        static const char kIni[] =
            "\xEF\xBB\xBF"          // BOM: the byte that used to eat [fix]
            "[fix]\r\n"
            "black_void = 1  # trailing comment, not part of the value\r\n"
            "share_exposure = YES\r\n"
            "panel_distance = 2.5   ; semicolon comment\r\n"
            "[d3d11]\r\n"
            "inventory = no\r\n"
            "[fix]\r\n"             // repeated, as the shipped file does it
            "fixture_value = 3\r\n"
            // A STRAY KEYSTROKE, which strtol used to read as a deliberate
            // setting. Found in a player ini as
            // transition_flash_max_consecutive = 3w: it parsed to 3, which
            // happens to equal the burst budget, so every excursion spent the
            // whole budget and opened a two-second window in which nothing
            // could be withheld. The flash the fix exists to hide came back,
            // and the cause was one invisible character.
            "fixture_stray = 4w\r\n"
            "panel_distance_index = 12 or 13\r\n"
            "black_void = 0\r\n";   // duplicate: the later one must win
        if (!writeIni(scratch, kIni)) {
            fail("scratch ini", "could not write it");
        } else {
            Config::get().init(scratch);
            expectBool("fix.share_exposure", true, "getBool accepts YES");
            expectBool("d3d11.inventory", false, "...and no");
            expectFloat("fix.panel_distance", 2.5f, "a ; comment is not part of the value");
            expectInt("fix.fixture_value", 3,
                      "a BOM does not swallow the first section");
            expectInt("fix.fixture_stray", -999999,
                      "a trailing letter is refused, not silently truncated");
            expectInt("fix.panel_distance_index", -999999,
                      "...and so is a value with words after the number");
            expectBool("fix.black_void", false,
                       "a duplicate key takes the later value");
        }

        // --- the config audit: moved keys and case, over fixture tables ---
        //
        // The DLLs register generated tables (config_audit.cpp); here small
        // fixture tables prove the mechanics without depending on which keys
        // happen to be moved this release. The scenario is 2026-08-27's
        // field bug exactly: new DLLs hand-copied over an old-layout ini,
        // where the value the user pinned sat under a section nothing read
        // any more -- and the compiled default (inherit) moved the whole
        // scanner UI.
        static const char* kKnown[] = {"fix.alpha", "experimental.beta"};
        static const char* kMoved[][3] = {
            {"fix.beta", "experimental.beta", ""},
            // A move whose old key shipped a default: a user line still
            // carrying that default is an un-updated file, not a choice, and
            // must NOT follow the move -- the new key's own default rules.
            // 2026-08-28's field bug exactly: menu_backdrop = stock (the old
            // shipped default) suppressing intro_backdrop's new splash.
            {"fix.gamma", "experimental.gamma", "stock"},
        };
        Config::get().setAuditTables(kKnown, 2, kMoved, 2);
        static const char kAuditIni[] =
            "[fix]\r\n"
            "beta = 7\r\n"          // the old-layout line: must follow the move
            "AlPhA = 3\r\n"         // case-typo: used to be filed unfindably
            "mystery = 9\r\n";      // a key nothing reads: named, not eaten
        if (!writeIni(scratch, kAuditIni)) {
            fail("audit scratch ini", "could not write it");
        } else {
            Config::get().init(scratch);
            expectInt("experimental.beta", 7,
                      "an old-layout value is read through the moved-from map");
            expectInt("fix.alpha", 3, "keys are matched case-insensitively");
            expectInt("fix.mystery", 9,
                      "an unknown key still reads (it is named in the log)");
        }
        static const char kBothIni[] =
            "[fix]\r\n"
            "beta = 7\r\n"
            "[experimental]\r\n"
            "beta = 5\r\n";
        if (!writeIni(scratch, kBothIni)) {
            fail("audit both-set ini", "could not write it");
        } else {
            Config::get().init(scratch);
            expectInt("experimental.beta", 5,
                      "when old and new are both set, the new name wins");
        }
        static const char kStaleIni[] =
            "[fix]\r\n"
            "gamma = stock\r\n"      // the OLD key's shipped default: stale
            "beta = screen\r\n";     // a real choice on a move with no default
        if (!writeIni(scratch, kStaleIni)) {
            fail("audit stale-default ini", "could not write it");
        } else {
            Config::get().init(scratch);
            expectStr("experimental.gamma", "<unset>",
                      "an old line carrying its retired default does not "
                      "follow the move; the caller's own default rules");
            expectStr("experimental.beta", "screen",
                      "a real old-line choice still follows the move");
        }
        Config::get().setAuditTables(nullptr, 0, nullptr, 0);

        // --- the demoted field-of-view trims, over the SHIPPED tables --------
        //
        // 2026-09-29: fix.fov_trim_vertical / _outer / _nasal moved to
        // [experimental] (edvr.ini: `# moved-from: fix.fov_trim_*`). Somebody who
        // set them has them under [fix] in an old-layout edvr.ini, and the DLLs
        // meet that file whenever they are copied in by hand. The fixture tables
        // above prove the mechanics; these are the tables the DLLs register --
        // config_contract_gen.h, generated from the real edvr.ini -- so what is
        // proven is that the keys which actually moved read through, and that it
        // is the shipped annotation that carries them.
        {
            using contractgen::kKnownKeys;
            using contractgen::kMovedKeys;
            constexpr size_t kKnownCount = sizeof(kKnownKeys) / sizeof(kKnownKeys[0]);
            constexpr size_t kMovedCount = sizeof(kMovedKeys) / sizeof(kMovedKeys[0]);
            constexpr const char* kTrimList = "pimax-openxr/pimax-crystal-super:10, "
                                              "virtualdesktopxr/meta-quest-3:5";
            static const char kOldTrims[] =
                "[fix]\r\n"
                "fov_trim_vertical = pimax-openxr/pimax-crystal-super:10, "
                "virtualdesktopxr/meta-quest-3:5\r\n"
                "fov_trim_outer = oculus/meta-quest-3:7\r\n"
                "fov_trim_nasal = pimax-openxr/pimax-crystal-super:5\r\n";

            // The tables carry the three moves, each old [fix] name to its new
            // [experimental] one, and nothing is left documented under the old.
            int trimMoves = 0;
            static const char* filtered[kMovedCount][3];
            size_t filteredCount = 0;
            for (size_t i = 0; i < kMovedCount; ++i) {
                const std::string oldKey = kMovedKeys[i][0], newKey = kMovedKeys[i][1];
                const bool trim = newKey.rfind("experimental.fov_trim_", 0) == 0;
                if (trim && oldKey == "fix.fov_trim_" + newKey.substr(strlen("experimental.fov_trim_"))) {
                    ++trimMoves;
                } else {
                    for (int c = 0; c < 3; ++c) filtered[filteredCount][c] = kMovedKeys[i][c];
                    ++filteredCount;
                }
            }
            if (trimMoves == 3) ok("the shipped moved-from map carries fix.fov_trim_* to experimental.fov_trim_*");
            else fail("the shipped moved-from map carries the three trims",
                      std::to_string(trimMoves) + " of 3 moves found in edvr.ini's annotations");
            int retiredStillKnown = 0;
            for (size_t i = 0; i < kKnownCount; ++i) {
                if (strncmp(kKnownKeys[i], "fix.fov_trim_", 13) == 0) ++retiredStillKnown;
            }
            if (retiredStillKnown == 0) ok("no fix.fov_trim_* key is still read or documented");
            else fail("no fix.fov_trim_* key is still read or documented",
                      std::to_string(retiredStillKnown) + " of them are");

            Config::get().setAuditTables(kKnownKeys, kKnownCount, kMovedKeys, kMovedCount);
            if (!writeIni(scratch, kOldTrims)) {
                fail("old-layout fov trim ini", "could not write it");
            } else {
                Config::get().init(scratch);
                expectStr("experimental.fov_trim_vertical", kTrimList,
                          "an old-layout fov_trim_vertical is read as experimental.fov_trim_vertical");
                expectStr("experimental.fov_trim_outer", "oculus/meta-quest-3:7",
                          "an old-layout fov_trim_outer is read as experimental.fov_trim_outer");
                expectStr("experimental.fov_trim_nasal", "pimax-openxr/pimax-crystal-super:5",
                          "an old-layout fov_trim_nasal is read as experimental.fov_trim_nasal");

                // Live: an edit to the OLD line under a running game reaches
                // the new name on the next reload, because the read-through is
                // resolved at every parse.
                if (!rewriteIni(scratch, "[fix]\r\nfov_trim_outer = oculus/meta-quest-3:9\r\n") ||
                    !Config::get().reloadIfChanged()) {
                    fail("old-layout fov trim reload", "the edit did not reload");
                } else {
                    expectStr("experimental.fov_trim_outer", "oculus/meta-quest-3:9",
                              "an edit to the old line is live: the next reload reads it");
                    expectStr("experimental.fov_trim_vertical", "<unset>",
                              "...and a trim no longer in the file is gone, not remembered");
                }
                // The shipped default, empty, in an old-layout file: no trim.
                if (!rewriteIni(scratch, "[fix]\r\nfov_trim_vertical =\r\n") ||
                    !Config::get().reloadIfChanged()) {
                    fail("old-layout empty fov trim reload", "the edit did not reload");
                } else {
                    expectStr("experimental.fov_trim_vertical", "",
                              "an old-layout line left empty reads as no trim");
                }
                // Both spellings in one file: the new name wins.
                if (!rewriteIni(scratch,
                                "[fix]\r\nfov_trim_outer = oculus/meta-quest-3:9\r\n"
                                "[experimental]\r\nfov_trim_outer = oculus/meta-quest-3:2\r\n") ||
                    !Config::get().reloadIfChanged()) {
                    fail("both-layout fov trim reload", "the edit did not reload");
                } else {
                    expectStr("experimental.fov_trim_outer", "oculus/meta-quest-3:2",
                              "when both spellings are set, experimental.fov_trim_outer wins");
                }
                // A new-layout file: read directly, and nothing under the old name.
                if (!rewriteIni(scratch, "[experimental]\r\nfov_trim_vertical = oculus:3\r\n") ||
                    !Config::get().reloadIfChanged()) {
                    fail("new-layout fov trim reload", "the edit did not reload");
                } else {
                    expectStr("experimental.fov_trim_vertical", "oculus:3",
                              "a new-layout fov_trim_vertical is read as it stands");
                    expectStr("fix.fov_trim_vertical", "<unset>",
                              "...and the retired name reads nothing");
                }
            }

            // CONTROL: the same old-layout file, over the same tables minus the
            // three moves (what edvr.ini without its annotations would generate).
            // The values are stranded under [fix] and the new names read nothing,
            // which is exactly what the assertions above would report if the
            // annotations were lost.
            Config::get().setAuditTables(kKnownKeys, kKnownCount,
                                         reinterpret_cast<const char* const(*)[3]>(filtered),
                                         filteredCount);
            if (!writeIni(scratch, kOldTrims)) {
                fail("control fov trim ini", "could not write it");
            } else {
                Config::get().init(scratch);
                expectStr("experimental.fov_trim_vertical", "<unset>",
                          "control: without the annotation an old-layout fov_trim_vertical is NOT read as the new name");
                expectStr("experimental.fov_trim_outer", "<unset>",
                          "control: ...nor fov_trim_outer");
                expectStr("experimental.fov_trim_nasal", "<unset>",
                          "control: ...nor fov_trim_nasal");
                expectStr("fix.fov_trim_outer", "oculus/meta-quest-3:7",
                          "control: the value sits under the old name, where nothing reads it");
            }
            Config::get().setAuditTables(nullptr, 0, nullptr, 0);
        }

        // --- the log directory, and the environment's say over it ----------
        //
        // build.bat's test runner gives each rig's proxies their own log
        // directory through EDVR_LOG_DIR, scoped by EDVR_LOG_DIR_FOR to the
        // exes in build\, so two rigs' proxies never share crash sentinels.
        // The scope matters: rigs that stage children in private directories
        // read those children's exe-relative logs, and the runner's variables
        // reach the children too. This process runs under those variables
        // itself, so each case sets both and the originals are put back.
        {
            const std::wstring exeDir = executableDirectory();
            const std::wstring exeDefault = exeDir + L"\\edvr_logs";
            const std::wstring moved = scratch + L"\\moved_logs";
            wchar_t savedDir[MAX_PATH]{}, savedFor[MAX_PATH]{};
            const bool hadDir = GetEnvironmentVariableW(L"EDVR_LOG_DIR", savedDir, MAX_PATH) != 0;
            const bool hadFor = GetEnvironmentVariableW(L"EDVR_LOG_DIR_FOR", savedFor, MAX_PATH) != 0;
            auto expectLogDir = [&](const wchar_t* dir, const wchar_t* onlyFor,
                                    const std::wstring& want, const char* what) {
                SetEnvironmentVariableW(L"EDVR_LOG_DIR", dir);
                SetEnvironmentVariableW(L"EDVR_LOG_DIR_FOR", onlyFor);
                Config::get().init(scratch);
                const std::wstring& got = Config::get().logDir();
                if (_wcsicmp(got.c_str(), want.c_str()) == 0) { ok(what); return; }
                std::string detail = "log dir is ";
                for (wchar_t c : got) detail.push_back(static_cast<char>(c));
                fail(what, detail);
            };
            std::wstring upperExeDir = exeDir;
            for (wchar_t& c : upperExeDir) c = static_cast<wchar_t>(towupper(c));
            if (!writeIni(scratch, "[fix]\r\nblack_void = 1\r\n")) {
                fail("log dir scratch ini", "could not write it");
            } else {
                expectLogDir(nullptr, nullptr, exeDefault, "without log.dir the log goes beside the exe");
                expectLogDir(moved.c_str(), nullptr, moved, "EDVR_LOG_DIR alone moves the default anywhere");
                expectLogDir(moved.c_str(), scratch.c_str(), exeDefault,
                             "EDVR_LOG_DIR_FOR naming another directory leaves this exe's default alone");
                expectLogDir(moved.c_str(), upperExeDir.c_str(), moved,
                             "EDVR_LOG_DIR_FOR naming the exe's directory, any case, moves it");
                expectLogDir(L"", upperExeDir.c_str(), exeDefault, "an empty EDVR_LOG_DIR moves nothing");
            }
            if (!writeIni(scratch, "[log]\r\ndir = C:\\elsewhere\\logs\r\n")) {
                fail("log.dir scratch ini", "could not write it");
            } else {
                expectLogDir(moved.c_str(), nullptr, L"C:\\elsewhere\\logs",
                             "an explicit log.dir wins over the environment");
            }
            SetEnvironmentVariableW(L"EDVR_LOG_DIR", hadDir ? savedDir : nullptr);
            SetEnvironmentVariableW(L"EDVR_LOG_DIR_FOR", hadFor ? savedFor : nullptr);
        }
    }

    // --- the log's buffer, and that a lost line SAYS it was lost -----------
    //
    // WHY THIS IS HERE (2026-09-07). Log::append drops a line when more text
    // queues between two flusher passes than the buffer holds, counts it in
    // m_dropped -- and until this date NOTHING EVER PRINTED THAT COUNT. Six
    // draw censuses across two flights were read as complete when every one
    // had lost its tail at the old 1 MB cap, taking the intern table that
    // says what each @N refers to. The diagnosis went to draw_census.cpp
    // twice before it came to the logger.
    //
    // So the property under test is not "the buffer is big enough" -- that is
    // a number in edvr.ini -- but "a log that lost lines is not silent about
    // it". Driven at buffer_mb = 1, the floor, so the test stays fast.
    if (argc >= 3) {
        const std::wstring scratch = widen(argv[2]);
        static const char kLogIni[] =
            "[log]\r\n"
            "enabled = 1\r\n"
            "buffer_mb = 1\r\n"
            "max_mb = 64\r\n";
        if (!writeIni(scratch, kLogIni)) {
            fail("log scratch ini", "could not write it");
        } else {
            Config::get().init(scratch);
            Log::get().close();          // in case anything above opened one

            // Clear previous runs first. Each leaves about a megabyte, so
            // without this every build grows the scratch directory forever --
            // and it also makes "the newest match" below provably THIS run's
            // rather than whichever name sorted last.
            {
                WIN32_FIND_DATAW old{};
                HANDLE oh = FindFirstFileW((scratch + L"\\edvr_buftest_*.log").c_str(),
                                           &old);
                if (oh != INVALID_HANDLE_VALUE) {
                    do {
                        DeleteFileW((scratch + L"\\" + old.cFileName).c_str());
                    } while (FindNextFileW(oh, &old));
                    FindClose(oh);
                }
            }

            if (!Log::get().open(scratch, L"buftest")) {
                fail("log buffer", "the log would not open in the scratch dir");
            } else {
                // Well past 1 MB, and fast: the flusher sleeps 250 ms before
                // its first pass, so this whole burst lands in one buffer --
                // which is exactly the shape of a draw census.
                std::string filler(800, 'x');
                for (int i = 0; i < 4000; ++i) {
                    Log::get().note("burst %d %s", i, filler.c_str());
                }
                const uint64_t lost = Log::get().dropped();
                Log::get().close();      // joins the flusher, flushes both

                if (lost == 0) {
                    fail("log buffer", "4000 x ~800 bytes did not overrun a "
                                       "1 MB buffer; the cap is not being read");
                } else {
                    ok("a burst past the buffer drops lines and counts them");
                }

                // And the count reaches the FILE, which is the half that was
                // missing: a reader of the log must see the gap.
                WIN32_FIND_DATAW fd{};
                const std::wstring pat = scratch + L"\\edvr_buftest_*.log";
                HANDLE h = FindFirstFileW(pat.c_str(), &fd);
                std::string body;
                if (h != INVALID_HANDLE_VALUE) {
                    std::wstring newest = fd.cFileName;
                    while (FindNextFileW(h, &fd)) newest = fd.cFileName;
                    FindClose(h);
                    HANDLE f = CreateFileW((scratch + L"\\" + newest).c_str(),
                                           GENERIC_READ, FILE_SHARE_READ, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                           nullptr);
                    if (f != INVALID_HANDLE_VALUE) {
                        char chunk[65536];
                        DWORD got = 0;
                        while (ReadFile(f, chunk, sizeof(chunk), &got, nullptr) &&
                               got) {
                            body.append(chunk, got);
                        }
                        CloseHandle(f);
                    }
                }
                if (body.empty()) {
                    fail("log buffer", "could not read the log back");
                } else if (body.find("were DROPPED here") == std::string::npos) {
                    fail("log buffer",
                         "lines were dropped and the log does not say so -- "
                         "the exact silence this test exists to prevent");
                } else {
                    ok("the log says in its own text that lines were dropped");
                }
            }
        }
    }

    // --- log.max_mb: the default is 16 (2026-10-01; it was 4), and the cap does what it says --------------------------------------
    //
    // The shipped edvr.ini documents the default commented out ("#max_mb = 16"), so for every install that has not written the line the
    // CODE's fallback is the cap; tools\check_config_contract.py holds a commented default to the code's in the build. This pins the
    // number itself and then what the log does with it, at sizes that keep the test fast: the default must not stop a 1.3 MB session
    // (past a 1 MB cap), an explicit max_mb = 1 must stop it with the one line that says so, and an explicit max_mb = 4 keeps 4 and
    // 0 is no cap (those two by the cap in force, with no writing). The writer has no rotation: a log that reaches its cap says so
    // once and stops, and the next session opens a new file; the flat profile reads this key the same way (the "log." keys are
    // allowed in both profiles, and the fallback is one line of one class).
    {
        const std::string iniText = readRepoFile(dir, L"edvr.ini");
        const std::string logSource = readRepoFile(dir, L"src\\common\\log.cpp");
        if (iniText.empty() || logSource.empty()) {
            fail("log.max_mb pins", "could not read edvr.ini or src\\common\\log.cpp from the repo root");
        } else {
            check(iniText.find("\n#max_mb = 16") != std::string::npos && iniText.find("\n#max_mb = 4") == std::string::npos,
                  "the shipped edvr.ini documents log.max_mb at 16, commented out (the code's fallback is the default)");
            check(logSource.find("getInt(\"log.max_mb\", 16)") != std::string::npos &&
                      logSource.find("getInt(\"log.max_mb\", 4)") == std::string::npos,
                  "the log's fallback for log.max_mb is 16");
        }
    }
    if (argc >= 3) {
        const std::wstring scratch = widen(argv[2]);
        struct CapRun {
            bool opened = false;
            uint64_t maxBytes = ~0ull;
            std::string body;
        };
        // One session in the scratch directory: the given [log] lines (a max_mb line or none), one 1.3 MB burst (the flusher's first pass
        // writes all of it), then a marker line a second pass can only write if the cap has not been reached. `write` false opens and
        // closes without writing, to read the cap in force.
        const auto session = [&](const char* maxMbLine, const wchar_t* tag, bool write) {
            CapRun run;
            std::string ini = "[log]\r\nenabled = 1\r\nbuffer_mb = 16\r\n";
            ini += maxMbLine;
            if (!writeIni(scratch, ini.c_str())) return run;
            Config::get().init(scratch);
            Log::get().close();
            const std::wstring pattern = std::wstring(L"edvr_") + tag + L"_*.log";
            {
                WIN32_FIND_DATAW old{};
                HANDLE oh = FindFirstFileW((scratch + L"\\" + pattern).c_str(), &old);
                if (oh != INVALID_HANDLE_VALUE) {
                    do {
                        DeleteFileW((scratch + L"\\" + old.cFileName).c_str());
                    } while (FindNextFileW(oh, &old));
                    FindClose(oh);
                }
            }
            if (!Log::get().open(scratch, tag)) return run;
            run.opened = true;
            run.maxBytes = Log::get().maxBytes();
            if (write) {
                const std::string filler(1000, 'y');
                for (int i = 0; i < 1300; ++i) Log::get().note("capfill %d %s", i, filler.c_str());
                Sleep(700);   // the flusher passes every 250 ms: one writes the burst, the next has to decide about the marker
                Log::get().note("capmarker after the first burst");
                Sleep(700);
            }
            Log::get().close();
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW((scratch + L"\\" + pattern).c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                const std::wstring name = fd.cFileName;
                FindClose(h);
                HANDLE f = CreateFileW((scratch + L"\\" + name).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f != INVALID_HANDLE_VALUE) {
                    char chunk[65536];
                    DWORD got = 0;
                    while (ReadFile(f, chunk, sizeof(chunk), &got, nullptr) && got) run.body.append(chunk, got);
                    CloseHandle(f);
                }
            }
            return run;
        };
        const auto count = [](const std::string& text, const char* needle) {
            size_t n = 0, from = 0;
            while ((from = text.find(needle, from)) != std::string::npos) {
                ++n;
                from += 1;
            }
            return n;
        };
        const char* const kCapLine = "log size cap reached; nothing further will be written";
        const uint64_t kMiB = 1024ull * 1024ull;

        const CapRun byDefault = session("", L"capdefault", true);
        if (!byDefault.opened) {
            fail("log.max_mb default", "the log would not open in the scratch dir");
        } else {
            check(byDefault.maxBytes == 16 * kMiB, "with no max_mb line the cap in force is 16 MB",
                  std::to_string(byDefault.maxBytes) + " bytes");
            check(count(byDefault.body, "capmarker after the first burst") == 1 && count(byDefault.body, kCapLine) == 0,
                  "...and a 1.3 MB session is not stopped by it (the old 4 MB default would not have stopped it either; a 1 MB cap does)");
        }
        const CapRun small = session("max_mb = 1\r\n", L"capsmall", true);
        if (!small.opened) {
            fail("log.max_mb = 1", "the log would not open in the scratch dir");
        } else {
            check(small.maxBytes == 1 * kMiB, "an explicit max_mb = 1 is a 1 MB cap", std::to_string(small.maxBytes) + " bytes");
            check(count(small.body, kCapLine) == 1 && count(small.body, "capmarker after the first burst") == 0,
                  "a session past its cap says so exactly once and writes nothing after it (the cap is the truncation: there is no rotation)");
            check(small.body.size() > 1 * kMiB && small.body.size() < 2 * kMiB,
                  "...and the file ends at the burst that crossed the cap (past 1 MB, under 2 MB), not at the default's 16 MB",
                  std::to_string(small.body.size()) + " bytes");
        }
        const CapRun four = session("max_mb = 4\r\n", L"capfour", false);
        check(four.opened && four.maxBytes == 4 * kMiB, "an ini that says max_mb = 4 keeps 4 MB", std::to_string(four.maxBytes) + " bytes");
        const CapRun none = session("max_mb = 0\r\n", L"capnone", false);
        check(none.opened && none.maxBytes == 0, "max_mb = 0 is no cap", std::to_string(none.maxBytes) + " bytes");
        Log::get().close();
    }

    // --- floats, notes and threads (2026-09-29) ----------------------------
    //
    // Before the profile cases below: those switch g_runtimeProfile to flat and
    // invalid, under which these scratch keys would read as suppressed.
    iniNameScan(dir);
    retiredKeyScan(dir);
    if (argc >= 3) {
        const std::wstring scratch = widen(argv[2]);
        floatCases(scratch);
        noteCases(scratch);
        iniNameCases(scratch);
        raceCase(scratch);
        shareDeleteCase(scratch);
    }

    // An old/full INI cannot widen a flat installation, even through numeric
    // getters whose ordinary fallback or lower bound would turn a fix on.
    const RuntimeProfile savedProfile = g_runtimeProfile;
    const char* descriptor = "[install]\r\nschema = 1\r\nprofile = flat\r\n";
    if (parseRuntimeProfile(descriptor) != RuntimeProfile::Flat ||
        parseRuntimeProfile("[install]\nschema=1\nprofile=vr\n") != RuntimeProfile::Vr ||
        parseRuntimeProfile("[install]\nschema=2\nprofile=flat\n") != RuntimeProfile::Invalid ||
        parseRuntimeProfile("[install]\nschema=1\nprofile=flat\nprofile=vr\n") != RuntimeProfile::Invalid ||
        parseRuntimeProfile("[install]\nprofile=flat\n") != RuntimeProfile::Invalid ||
        parseRuntimeProfile(std::string(4097, 'x')) != RuntimeProfile::Invalid)
        fail("profile descriptor", "invalid schema/duplicate/oversize admitted");
    else ok("profile descriptor refuses ambiguous or unsupported scope");
    g_runtimeProfile = RuntimeProfile::Flat;
    // The flat adapter reads this through getString with an on default. A
    // refused key turns that default into off, so exercise the actual getter.
    if (argc >= 3) {
        const std::wstring scratch = widen(argv[2]);
        if (!writeIni(scratch, "[fix]\r\nblack_void = on\r\n"))
            fail("flat jitter scratch ini", "could not write it");
        else {
            Config::get().init(scratch);
            g_runtimeProfile = RuntimeProfile::Flat;
            // The flat jitter cycle's key is read through getInt with an 8 default; a refused key would answer 0.
            if (Config::get().getInt("advanced.temporal_aa_jitter_phases", 8) == 8)
                ok("flat jitter cycle uses the 8 default when absent");
            else fail("flat jitter cycle default", "missing key was refused");
            if (Config::get().getBool("advanced.input_gate", true))
                ok("flat input_gate uses true default when absent");
            else fail("flat input_gate default", "missing key was not true");
            // The interface quality (2026-10-09): a flat panel row, 100 by default. Refused, the default would read off and an
            // edvr-flat.ini with no line (every one written before the row) would get today's interface with nothing said.
            if (Config::get().getString("fix.ui_quality", "100") == "100")
                ok("flat ui_quality uses the 100 default when absent");
            else fail("flat ui_quality default", "missing key was not 100");
        }
    }
    Config::get().set("advanced.input_gate", "off");
    expectBool("advanced.input_gate", false, "flat scope reads input_gate override off");
    Config::get().set("advanced.input_gate", "on");
    expectBool("advanced.input_gate", true, "flat scope reads input_gate override on");
    // The flat panel's UI quality row reads fix.ui_quality through getString: it must pass the gate both ways.
    Config::get().set("fix.ui_quality", "125");
    expectStr("fix.ui_quality", "125", "flat scope permits the UI quality key");
    Config::get().set("fix.ui_quality", "off");
    expectStr("fix.ui_quality", "off", "flat scope reads the UI quality key off");
    Config::get().set("fix.ui_quality", "100");
    // The flat jitter cycle's length (advanced.temporal_aa_jitter_phases): the flat runtime reads it through getInt with an 8 default.
    // Unlisted in runtimeProfileAllowsKey, a refused key answers 0 whatever the file says, which the reader takes for out of range and
    // reads as 8: a user who wrote 32 would fly 8, with a log line naming a value they never wrote.
    Config::get().set("advanced.temporal_aa_jitter_phases", "32");
    expectInt("advanced.temporal_aa_jitter_phases", 32, "flat scope permits the jitter cycle's key");
    Config::get().set("advanced.temporal_aa_jitter_phases", "8");
    expectInt("advanced.temporal_aa_jitter_phases", 8, "flat scope reads the jitter cycle's key at its default");
    // The VR camera census's key is a VR-profile key too: a flat profile must never install its hook.
    Config::get().set("advanced.vr_camera_census", "on");
    expectStr("advanced.vr_camera_census", "off", "flat scope refuses the VR camera census's key");
    Config::get().set("fix.temporal_aa", "dlss");
    Config::get().set("fix.black_void", "on");
    Config::get().set("fix.explorer_cam_eye_forward", "12");
    Config::get().set("experimental.night_vision_realistic", "on");
    Config::get().set("advanced.real_dll", "d3d11_edhm.dll");
    expectStr("fix.temporal_aa", "off", "flat discovery cannot activate stereo temporal AA");
    if (Config::get().requestedTemporalMode() != "dlss") fail("flat request", "intent lost");
    else ok("flat diagnostic retains requested temporal mode");
    expectBool("fix.black_void", false, "flat profile suppresses restored unrelated fix");
    expectBool("experimental.night_vision_realistic", false,
               "flat jitter exception leaves unrelated experimental settings suppressed");
    // Night vision's pulse stability defaults ON, and nightVisionConfigure reads it through
    // this getter: a refused key reading off is all that keeps nightVisionWantsDraws() false on
    // flat, so the draw gate (vscreen.cpp drawGateSubscribed) is not held open there by a fix
    // the flat profile does not run. expectBool asks with both defaults, so it is the default-on
    // read that is pinned.
    Config::get().set("fix.night_vision_stability", "on");
    expectBool("fix.night_vision_stability", false,
               "flat profile: night vision pulse stability (default on) reads off, so it cannot hold the draw gate open");
    // The depth probe is armed by fix.temporal_aa (read off on flat, above) or by the eye depth capture,
    // and depthProbeConfigure reads the capture through this getter: a refused key reading off is what
    // keeps depthProbeWanted() false on flat, so the draw gate (vscreen.cpp drawGateSubscribed) is not
    // held open there by a probe the flat profile does not run.
    Config::get().set("advanced.eye_depth_capture", "on");
    expectBool("advanced.eye_depth_capture", false,
               "flat profile: eye depth capture reads off, so it cannot arm the depth probe or hold the draw gate open");
    expectInt("fix.explorer_cam_eye_forward", 0, "flat profile suppresses numeric fix");
    expectFloat("fix.explorer_cam_eye_forward", 0.0f, "flat profile suppresses float fix");
    if (Config::get().getIntInRange("fix.explorer_cam_eye_forward", 12, 1, 100) != 0)
        fail("flat bounded getter", "minimum reactivated disabled feature");
    else ok("flat scope precedes bounded numeric defaults");
    expectStr("advanced.real_dll", "d3d11_edhm.dll", "flat scope preserves mod chaining");
    Config::get().set("hotkey.menu", "F8");
    expectStr("hotkey.menu", "F8", "flat scope permits the temporal menu hotkey");
    Config::get().set("fix.temporal_aa_model", "m");
    expectStr("fix.temporal_aa_model", "m", "flat scope permits the DLSS model");
    // The flat panel's Sharpening row and the flat sharpening both read this key
    // through the generic getter. Unlisted, it would read 0 here whatever the file
    // says: no error, no log line, a row that does nothing.
    Config::get().set("fix.render_sharpness", "0.3");
    expectFloat("fix.render_sharpness", 0.3f, "flat scope permits the sharpening setting");
    expectStr("hotkey.toggle_exposure", "", "flat menu exception leaves unrelated hotkeys suppressed");
    const struct { const char* name; unsigned full, fovea; bool known; } presets[] = {
        {"auto",0,0,true},{"default",0,0,true},{"j",10,10,true},
        {"K",11,11,true},{"l",12,12,true},{"m",13,13,true},
        {"quality",11,11,true},{"steady",11,12,true},{"responsive",10,10,true},
        {"removed-preset",11,11,false}
    };
    for (const auto& p : presets) {
        const auto actual=temporalPresetFor(p.name);
        if (actual.full!=p.full || actual.known!=p.known)
            fail("shared temporal preset mapping",p.name);
    }
    Config::get().set("fix.black_void", "on");
    expectBool("fix.black_void", false, "live setting change cannot widen scope");
    g_runtimeProfile = RuntimeProfile::Invalid;
    expectBool("advanced.d3d11_fixes", false, "bad descriptor disables graphics hooks");
    expectStr("hotkey.menu", "", "invalid profile cannot open temporal menu");
    expectStr("fix.temporal_aa_model", "off", "invalid profile cannot select a DLSS model");
    expectFloat("fix.render_sharpness", 0.0f, "invalid profile cannot sharpen");
    expectStr("advanced.real_dll", "d3d11_edhm.dll", "bad descriptor preserves mod chaining");
    Config::get().set("advanced.temporal_aa_jitter_phases", "32");
    expectInt("advanced.temporal_aa_jitter_phases", 0, "invalid profile refuses the flat jitter cycle's key");
    Config::get().set("advanced.vr_camera_census", "on");
    expectStr("advanced.vr_camera_census", "off", "invalid profile cannot turn the VR camera census on");
    g_runtimeProfile = RuntimeProfile::LegacyVr;
    expectBool("fix.black_void", true, "legacy profile retains original behavior");
    expectInt("advanced.temporal_aa_jitter_phases", 32, "legacy profile reads the flat jitter cycle's key as written");
    g_runtimeProfile = RuntimeProfile::Vr;
    Config::get().set("advanced.vr_camera_census", "on");
    expectStr("advanced.vr_camera_census", "on", "VR profile reads the VR camera census's explicit on");
    g_runtimeProfile = RuntimeProfile::LegacyVr;
    expectStr("advanced.vr_camera_census", "on", "the legacy VR profile reads it too (runtimeVrProfile covers both)");
    g_runtimeProfile = RuntimeProfile::Vr;
    expectFloat("fix.render_sharpness", 0.3f, "VR profile still reads the sharpening setting");

    // The flat panel writes the Sharpening row into edvr-flat.ini and asks for a
    // reload; the flat sharpening reads the key every frame. Live means that file,
    // read first (config.cpp), moves the value on the next reload -- not the
    // edvr.ini beside it, which a flat install falls back to and a VR one owns.
    if (argc >= 3) {
        const std::wstring flatDir = widen(argv[2]) + L"_flatsharp";
        g_runtimeProfile = RuntimeProfile::Flat;
        const wchar_t* flatLeaf = L"edvr-flat.ini";
        // The descriptor beside the inis is what makes init() pick the flat file:
        // without it the profile reads as the legacy VR one and edvr.ini wins.
        if (!writeIni(flatDir, descriptor, L"edvr_profile.ini") ||
            !rewriteIni(flatDir, "[fix]\r\nrender_sharpness = 0.9\r\n") ||
            !rewriteIni(flatDir, "[fix]\r\nrender_sharpness = 0.4\r\n", flatLeaf)) {
            fail("flat sharpening reload", "could not write the scratch inis");
        } else {
            Config::get().init(flatDir);
            if (!runtimeFlatProfile()) fail("flat sharpening reload", "the descriptor did not select flat");
            expectFloat("fix.render_sharpness", 0.4f,
                        "the flat file is read first: its 0.4, not edvr.ini's 0.9");
            if (!rewriteIni(flatDir, "[fix]\r\nrender_sharpness = 0\r\n", flatLeaf) ||
                !Config::get().reloadIfChanged())
                fail("flat sharpening reload", "a panel write of 0 did not reload");
            else expectFloat("fix.render_sharpness", 0.0f, "a panel write of 0 is live");
            if (!rewriteIni(flatDir, "[fix]\r\nrender_sharpness = 0.65\r\n", flatLeaf) ||
                !Config::get().reloadIfChanged())
                fail("flat sharpening reload", "a panel write of 0.65 did not reload");
            else expectFloat("fix.render_sharpness", 0.65f, "a panel write of 0.65 is live");
        }
    }
    g_runtimeProfile = savedProfile;

    if (g_fails) {
        printf("CONFIG TEST FAILED (%d)\n", g_fails);
        return 1;
    }
    printf("CONFIG TEST PASSED\n");
    return 0;
}
