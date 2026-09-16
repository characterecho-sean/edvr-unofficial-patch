#include "intro_skip.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include <windows.h>

#include "../common/config.h"
#include "../common/iat_hook.h"
#include "../common/intro_mode.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "intro_probe.h"   // the device stamp the watch's "+X.XXX s" reads

namespace edvr {
namespace {

using PFN_CreateFileW = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD,
                                        DWORD, HANDLE);
using PFN_CreateFileA = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD,
                                        DWORD, HANDLE);
using PFN_GetFileAttributesW = DWORD(WINAPI*)(LPCWSTR);
using PFN_GetFileAttributesA = DWORD(WINAPI*)(LPCSTR);
using PFN_GetFileAttributesExW = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using PFN_GetFileAttributesExA = BOOL(WINAPI*)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);

// Read on whatever thread the game opens files from. Everything else below
// belongs to the render thread, which is where configure and tick run.
std::atomic<bool> g_armed{false};
// The WATCH (advanced.intro_probe, with the movie not skipped): the same
// hooks, installed forwarding, so the first open of an ident is timed
// against the device line. It refuses nothing, ever. Off whenever the skip
// is armed -- the refusal lines say what opened then.
std::atomic<bool> g_watching{false};
bool     g_installTried = false;
bool     g_installed = false;
bool     g_armedSaid = false;     // the ARMED line has been said this session
IatPatch g_createW, g_createA, g_attrW, g_attrA, g_attrExW, g_attrExA;
IatPatch g_readerCreateW;
HMODULE g_reader = nullptr;

std::atomic<uint32_t> g_refused{0};          // ident opens and attribute reads refused
std::atomic<uint32_t> g_otherMovieOpens{0};  // Movies\ paths that were not idents, forwarded
std::atomic<uint32_t> g_identOpens{0};       // ident opens seen by the WATCH, forwarded
// The "the game opened" line's once-guard, per API family (N5, 2026-09-15):
// a GetFileAttributes* existence check must not consume the line the
// DirectShow reader's actual CreateFile* open (the decoder-clock proxy)
// earns. g_identOpens above still counts every touch, across both families.
std::atomic<bool> g_identOpenNotedCreate{false};
std::atomic<bool> g_identOpenNotedAttr{false};
std::atomic<uint32_t> g_refusalLines{0};     // the log lines spent on refusals
std::atomic<uint32_t> g_movieDrew{0};        // frames the movie's fill drew
uint64_t g_armedAtMs = 0;
bool     g_drewSaid = false;
bool     g_verdictSaid = false;
bool     g_watchRetired = false;  // the watch's scene-frame account has been said
bool     g_sceneSeen = false;     // the first rendered scene has passed the tick
bool     g_watchLateSaid = false; // the watch was asked for too late; said once

enum class MoviePath { none, other, ident };

// What a path is to this module, for either character width. Only the last
// two components are read: a directory called `movies` (either separator,
// any prefix, any case) and then a file name that starts `ident_` or is
// `intro_temp.webm`. FrontEnd*.webm -- the menu's own loops -- and everything
// else under the folder are `other`. Bounded and exception-free, touching
// nothing but the string: it runs inside the game's CreateFileW.
template <class C>
MoviePath classify(const C* p) {
    if (!p) return MoviePath::none;
    size_t len = 0;
    while (p[len]) {
        if (++len > 4096) return MoviePath::none;
    }
    size_t nameStart = 0;
    for (size_t i = 0; i < len; ++i) {
        if (p[i] == '\\' || p[i] == '/') nameStart = i + 1;
    }
    if (nameStart == 0 || nameStart >= len) return MoviePath::none;
    size_t dirStart = nameStart - 1;
    while (dirStart > 0 && p[dirStart - 1] != '\\' && p[dirStart - 1] != '/') --dirStart;
    auto lower = [](C c) -> unsigned {
        if (c >= 'A' && c <= 'Z') return static_cast<unsigned>(c - 'A' + 'a');
        return static_cast<unsigned>(c);
    };
    auto matches = [&](size_t at, size_t count, const char* word) {
        for (size_t i = 0; i < count; ++i) {
            if (at + i >= len || lower(p[at + i]) != static_cast<unsigned>(word[i])) return false;
        }
        return true;
    };
    if (nameStart - 1 - dirStart != 6 || !matches(dirStart, 6, "movies")) return MoviePath::none;
    const size_t nameLen = len - nameStart;
    if (nameLen >= 6 && matches(nameStart, 6, "ident_")) return MoviePath::ident;
    if (nameLen == 15 && matches(nameStart, 15, "intro_temp.webm")) return MoviePath::ident;
    return MoviePath::other;
}

void noteRefusal(const char* api, const wchar_t* path) {
    Log::get().note("intro skip: refused the game's %s of %ls -- answered 'file not "
                    "found', as a renamed file would be.", api, path);
}
void noteRefusal(const char* api, const char* path) {
    Log::get().note("intro skip: refused the game's %s of %s -- answered 'file not "
                    "found', as a renamed file would be.", api, path);
}

// The file name alone, for the watch line: the path the game hands over is
// the full install path, and the name is what identifies the ident.
template <class C>
const C* baseName(const C* p) {
    const C* name = p;
    for (const C* c = p; *c; ++c) {
        if (*c == '\\' || *c == '/') name = c + 1;
    }
    return name;
}

// True for the GetFileAttributes* family (an existence check), false for
// CreateFile* (an actual open, including the DirectShow reader's own). The
// two families get separate once-guards below, so an existence check
// cannot consume the line the real open earns.
bool isAttrApi(const char* api) {
    return std::strncmp(api, "GetFileAttributes", 17) == 0;
}

// The WATCH's line: the first ident open, timed against the device -- once
// per API family (N5, 2026-09-15), so a GetFileAttributes* existence check
// cannot swallow the DirectShow reader's actual CreateFile* open (the
// decoder-clock proxy). Runs inside the game's CreateFileW on whatever
// thread opens the movie, so it is bounded and touches nothing but the log.
// g_identOpens counts every touch, across both families; the exchange below
// is the per-family once-guard.
void noteWatchedOpen(const char* api, const wchar_t* path) {
    g_identOpens.fetch_add(1, std::memory_order_relaxed);
    std::atomic<bool>& noted = isAttrApi(api) ? g_identOpenNotedAttr : g_identOpenNotedCreate;
    if (noted.exchange(true, std::memory_order_relaxed)) return;
    double since = 0.0;
    if (introProbeSinceDevice(&since)) {
        Log::get().note("intro probe: the game opened %ls at +%.3f s after the device "
                        "(thread %lu, through %s); the movie's clock starts about here.",
                        baseName(path), since, GetCurrentThreadId(), api);
    } else {
        Log::get().note("intro probe: the game opened %ls before any D3D11 device existed "
                        "(thread %lu, through %s; read this line's timestamp against the "
                        "device line); the movie's clock starts about here.",
                        baseName(path), GetCurrentThreadId(), api);
    }
}
void noteWatchedOpen(const char* api, const char* path) {
    g_identOpens.fetch_add(1, std::memory_order_relaxed);
    std::atomic<bool>& noted = isAttrApi(api) ? g_identOpenNotedAttr : g_identOpenNotedCreate;
    if (noted.exchange(true, std::memory_order_relaxed)) return;
    double since = 0.0;
    if (introProbeSinceDevice(&since)) {
        Log::get().note("intro probe: the game opened %s at +%.3f s after the device "
                        "(thread %lu, through %s); the movie's clock starts about here.",
                        baseName(path), since, GetCurrentThreadId(), api);
    } else {
        Log::get().note("intro probe: the game opened %s before any D3D11 device existed "
                        "(thread %lu, through %s; read this line's timestamp against the "
                        "device line); the movie's clock starts about here.",
                        baseName(path), GetCurrentThreadId(), api);
    }
}

// True when the call is to be answered "not found": the skip is armed and
// the path is an ident. Counts what it sees either way, so the verdict can
// say whether this table carries the game's movie opens at all. The WATCH
// takes the same walk and never returns true: an ident it sees is noted
// and forwarded.
template <class C>
bool refuse(const C* path, const char* api) {
    const bool armed = g_armed.load(std::memory_order_relaxed);
    if (!armed && !g_watching.load(std::memory_order_relaxed)) return false;
    const MoviePath kind = classify(path);
    if (kind == MoviePath::other) {
        g_otherMovieOpens.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (kind != MoviePath::ident) return false;
    if (!armed) {
        noteWatchedOpen(api, path);
        return false;
    }
    g_refused.fetch_add(1, std::memory_order_relaxed);
    if (g_refusalLines.fetch_add(1, std::memory_order_relaxed) < 4) noteRefusal(api, path);
    return true;
}

HANDLE WINAPI hookCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                              DWORD disposition, DWORD flags, HANDLE templateFile) {
    if (refuse(name, "CreateFileW")) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    return reinterpret_cast<PFN_CreateFileW>(g_createW.original)(name, access, share, sa,
                                                                 disposition, flags, templateFile);
}

HANDLE WINAPI hookCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                              DWORD disposition, DWORD flags, HANDLE templateFile) {
    if (refuse(name, "CreateFileA")) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    return reinterpret_cast<PFN_CreateFileA>(g_createA.original)(name, access, share, sa,
                                                                 disposition, flags, templateFile);
}

HANDLE WINAPI hookReaderCreateFileW(LPCWSTR name, DWORD access, DWORD share,
                                    LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                                    DWORD flags, HANDLE templateFile) {
    if (refuse(name, "DirectShow CreateFileW")) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    return reinterpret_cast<PFN_CreateFileW>(g_readerCreateW.original)(
        name, access, share, sa, disposition, flags, templateFile);
}

DWORD WINAPI hookGetFileAttributesW(LPCWSTR name) {
    if (refuse(name, "GetFileAttributesW")) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_FILE_ATTRIBUTES;
    }
    return reinterpret_cast<PFN_GetFileAttributesW>(g_attrW.original)(name);
}

DWORD WINAPI hookGetFileAttributesA(LPCSTR name) {
    if (refuse(name, "GetFileAttributesA")) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_FILE_ATTRIBUTES;
    }
    return reinterpret_cast<PFN_GetFileAttributesA>(g_attrA.original)(name);
}

BOOL WINAPI hookGetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    if (refuse(name, "GetFileAttributesExW")) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    return reinterpret_cast<PFN_GetFileAttributesExW>(g_attrExW.original)(name, level, info);
}

BOOL WINAPI hookGetFileAttributesExA(LPCSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    if (refuse(name, "GetFileAttributesExA")) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    return reinterpret_cast<PFN_GetFileAttributesExA>(g_attrExA.original)(name, level, info);
}

const char* state(const IatPatch& p) { return p.applied ? "patched" : "not imported"; }

// The executable's own import slots, once, for the skip or for the watch --
// whichever asks first; the other finds them in place. Said before g_armed
// or g_watching goes true, so the ARMED or watching line precedes any
// refusal or open line in the log. `consequence` is what a failure means to
// the caller, since the two want different things from the hooks.
void install(const char* consequence) {
    g_installTried = true;
    iatHookInstall("kernel32.dll", "CreateFileW", reinterpret_cast<void*>(&hookCreateFileW),
                   &g_createW);
    iatHookInstall("kernel32.dll", "CreateFileA", reinterpret_cast<void*>(&hookCreateFileA),
                   &g_createA);
    iatHookInstall("kernel32.dll", "GetFileAttributesW",
                   reinterpret_cast<void*>(&hookGetFileAttributesW), &g_attrW);
    iatHookInstall("kernel32.dll", "GetFileAttributesA",
                   reinterpret_cast<void*>(&hookGetFileAttributesA), &g_attrA);
    iatHookInstall("kernel32.dll", "GetFileAttributesExW",
                   reinterpret_cast<void*>(&hookGetFileAttributesExW), &g_attrExW);
    iatHookInstall("kernel32.dll", "GetFileAttributesExA",
                   reinterpret_cast<void*>(&hookGetFileAttributesExA), &g_attrExA);
    // 09:43 flight: zero executable requests, 902 movie frames. Elite names
    // CLSID_AsyncReader, whose Windows implementation opens through quartz's
    // own IAT (reproduced with IFileSourceFilter::Load on the actual ident).
    // Load the system reader now so its slot is patched before graph setup;
    // keep our module reference until the hook has been removed.
    g_reader = LoadLibraryExW(L"quartz.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (g_reader) iatHookInstallIn(g_reader, "kernel32.dll", "CreateFileW",
        reinterpret_cast<void*>(&hookReaderCreateFileW), &g_readerCreateW);
    if (!g_readerCreateW.applied) Log::get().note(
        "intro skip: DirectShow reader hook unavailable (%s). Only executable imports "
        "are covered.", g_reader ? "CreateFileW not imported" : "system quartz.dll not loaded");
    g_installed = g_createW.applied || g_createA.applied || g_readerCreateW.applied;
    if (!g_installed) {
        Log::get().note(
            "intro skip: NOT installed -- no executable or DirectShow file-open "
            "import could be patched. %s", consequence);
    }
}

// The module the hooks forward into, for the lines that name the install.
void entryModule(char* mod, size_t len) {
    iatHookEntryModule(g_readerCreateW.applied ? g_readerCreateW.original :
                       g_createW.applied ? g_createW.original : g_createA.original, mod,
                       len);
}

void sayArmed() {
    char mod[64] = "?";
    entryModule(mod, sizeof(mod));
    Log::get().note(
        "intro skip: ARMED -- fix.intro_video = skip. The executable's imports: CreateFileW "
        "%s, CreateFileA %s, GetFileAttributesW %s, GetFileAttributesA %s, "
        "GetFileAttributesExW %s, GetFileAttributesExA %s; DirectShow reader CreateFileW %s "
        "(original entry in %s). An open of Movies\\Ident_*.webm or intro_temp.webm is "
        "answered 'file not found' -- the answer a renamed file gives it, so the game's own "
        "missing-movie path runs -- and nothing on disk is touched. Decided at launch. "
        "Only the executable and system reader import tables are patched. "
        "The verdict prints when the first rendered scene arrives (docs\\intro-video.md).",
        state(g_createW), state(g_createA), state(g_attrW), state(g_attrA), state(g_attrExW),
        state(g_attrExA), state(g_readerCreateW), mod);
}

void sayWatching() {
    char mod[64] = "?";
    entryModule(mod, sizeof(mod));
    Log::get().note(
        "intro probe: watching the movie's open -- the file hooks are installed and "
        "forwarding (executable CreateFileW %s, CreateFileA %s; DirectShow reader CreateFileW "
        "%s; original entry in %s). Nothing is refused. The first open of Movies\\Ident_*.webm "
        "or intro_temp.webm is timed against the device line; a session with this line and "
        "no 'the game opened' line opened the movie by a route these hooks do not see.",
        state(g_createW), state(g_createA), state(g_readerCreateW), mod);
}

}  // namespace

void introSkipConfigure(Config& cfg) {
    const IntroVideoMode mode = introVideoParse(cfg.getString("fix.intro_video", "screen"));
    // The watch is the probe's, and stands aside whenever the skip is armed:
    // a refused open is already a line of its own.
    const bool watch = cfg.getBool("advanced.intro_probe", false) && !mode.skip;
    const bool was = g_armed.load(std::memory_order_relaxed);
    if (mode.skip != was) {
        if (mode.skip) {
            if (!g_installTried) install("The movie will play as fix.intro_video = screen.");
            if (g_installed) {
                if (!g_armedSaid) {
                    g_armedSaid = true;
                    sayArmed();
                } else {
                    Log::get().note("intro skip: armed again. The movie is decided at launch, "
                                    "so this matters at the next one.");
                }
            }
            g_armedAtMs = nowMs();
            g_armed.store(g_installed, std::memory_order_release);
        } else {
            g_armed.store(false, std::memory_order_release);
            Log::get().note("intro skip: off -- the movie is the game's to open again (%u "
                            "refusal(s) this session).",
                            g_refused.load(std::memory_order_relaxed));
        }
    }
    const bool wasWatching = g_watching.load(std::memory_order_relaxed);
    if (watch == wasWatching) return;
    if (!watch) {
        g_watching.store(false, std::memory_order_release);
        Log::get().note("intro probe: the movie's open is no longer watched (%u ident "
                        "open(s) seen).",
                        g_identOpens.load(std::memory_order_relaxed));
        return;
    }
    // Asked for once the movie's open is behind us -- a reload after the
    // movie drew, or after the first rendered scene -- the watch has nothing
    // to see this launch, and installing it would let the scene edge print
    // 'NO ident was opened' about hooks that were not there at the open.
    // Declined, said once; the next launch reads the key at startup.
    const bool late = g_sceneSeen || g_movieDrew.load(std::memory_order_relaxed) != 0;
    if (late) {
        if (!g_watchLateSaid) {
            g_watchLateSaid = true;
            Log::get().note("intro probe: the movie's open watch was asked for after %s; "
                            "the movie is decided at launch, so it matters at the next one.",
                            g_sceneSeen ? "the first rendered scene"
                                        : "the movie had already drawn");
        }
        return;
    }
    if (!g_installTried) install("The intro probe cannot see the game's movie opens.");
    g_watching.store(g_installed, std::memory_order_release);
    if (g_installed) sayWatching();
}

void introSkipNoteMovieDrew() {
    g_movieDrew.fetch_add(1, std::memory_order_relaxed);
    // Once the scene has arrived the verdict is in, and a movie-shaped fill
    // after it is not the ident: the front end's own loops (FrontEnd*.webm)
    // ride the same composite, and the 10:09 flight of 2026-09-13 printed
    // "DRAWING anyway" 0.6 s after WORKED on exactly that. Counted, not said.
    if (!g_armed.load(std::memory_order_relaxed) || g_drewSaid || g_verdictSaid) return;
    g_drewSaid = true;
    const uint32_t refused = g_refused.load(std::memory_order_relaxed);
    if (refused == 0) {
        Log::get().note(
            "intro skip: the movie is DRAWING anyway -- its planes reached the composite and "
            "NOTHING was refused through the executable or DirectShow reader imports. "
            "The file may have opened before arming or through another reader. "
            "%u other Movies\\ open(s) reached these hooks. Please report this log.",
            g_otherMovieOpens.load(std::memory_order_relaxed));
        return;
    }
    Log::get().note(
        "intro skip: the movie is DRAWING anyway, after %u refusal(s): the game was told the "
        "file was not there and got it some other way. Please report this log.",
        refused);
}

void introSkipTick(bool sceneFrame) {
    if (!sceneFrame) return;
    g_sceneSeen = true;
    // The watch's account, once, at the same edge the verdict uses. Every
    // outcome is a line: an ident open with its count, or NO open, which is
    // Flight 09:43's shape and the finding the watch exists to name -- read
    // with the fill witness, since NO open and NO fill is the game skipping
    // the movie by itself, not a route the hooks miss.
    if (!g_watchRetired && g_watching.load(std::memory_order_relaxed)) {
        g_watchRetired = true;
        const uint32_t idents = g_identOpens.load(std::memory_order_relaxed);
        const uint32_t other = g_otherMovieOpens.load(std::memory_order_relaxed);
        const uint32_t drew = g_movieDrew.load(std::memory_order_relaxed);
        if (idents) {
            Log::get().note("intro probe: the movie's open watch retires at the first "
                            "rendered scene -- %u ident open(s) and %u other Movies\\ "
                            "open(s) reached the hooks, all forwarded; the movie drew %u "
                            "frame(s).",
                            idents, other, drew);
        } else if (drew) {
            Log::get().note("intro probe: the movie's open watch retires at the first "
                            "rendered scene -- NO ident was opened through the executable "
                            "or DirectShow reader imports (%u other Movies\\ open(s) "
                            "were), yet the movie drew %u frame(s): it was opened by a "
                            "route these hooks do not see.",
                            other, drew);
        } else {
            Log::get().note("intro probe: the movie's open watch retires at the first "
                            "rendered scene -- NO ident was opened through the executable "
                            "or DirectShow reader imports (%u other Movies\\ open(s) "
                            "were) and the movie never drew: the game skipped it by "
                            "itself this launch, or played it by a route neither the "
                            "hooks nor the fill detection covers.",
                            other);
        }
    }
    if (g_verdictSaid || !g_armed.load(std::memory_order_relaxed)) return;
    g_verdictSaid = true;
    const uint32_t refused = g_refused.load(std::memory_order_relaxed);
    const uint32_t drew = g_movieDrew.load(std::memory_order_relaxed);
    const uint32_t other = g_otherMovieOpens.load(std::memory_order_relaxed);
    const double since = static_cast<double>(nowMs() - g_armedAtMs) / 1000.0;
    if (refused && !drew) {
        Log::get().note(
            "intro skip: WORKED -- %u refusal(s), the movie never drew, and the first rendered "
            "scene arrived %.1f s after the skip armed (the movie alone runs about twenty "
            "seconds).",
            refused, since);
        return;
    }
    if (!refused && !drew) {
        Log::get().note(
            "intro skip: no ident was asked for through the executable or DirectShow reader and no "
            "movie drew (%u other Movies\\ open(s) seen; the scene arrived at %.1f s). Either "
            "the game skipped it by itself this launch, or it opened and played it by a route "
            "neither this hook nor the fill detection covers. Please report this log.",
            other, since);
        return;
    }
    Log::get().note("intro skip: did not take -- %u refusal(s), the movie drew %u frame(s), "
                    "the scene arrived at %.1f s.",
                    refused, drew, since);
}

void introSkipShutdown() {
    g_armed.store(false, std::memory_order_release);
    g_watching.store(false, std::memory_order_release);
    iatHookUninstall(&g_readerCreateW);
    if (g_reader) { FreeLibrary(g_reader); g_reader = nullptr; }
    iatHookUninstall(&g_attrExA);
    iatHookUninstall(&g_attrExW);
    iatHookUninstall(&g_attrA);
    iatHookUninstall(&g_attrW);
    iatHookUninstall(&g_createA);
    iatHookUninstall(&g_createW);
    g_installed = false;
    g_installTried = false;
}

}  // namespace edvr

// The desk half, for smoke.exe: the predicate against the paths the game
// could plausibly hand it, and the refusal's shape through the hook bodies
// themselves. A wrong predicate would not crash a flight, it would spend one
// -- the log would say "no ident was asked for" and mean nothing -- so the
// cells run before any flight does. Bits: 1 predicate, 2 refusal, 4 watch.
extern "C" __declspec(dllexport) unsigned edvrIntroSkipSelftest() {
    using namespace edvr;
    unsigned bits = 0;
    bool ok = true;
    auto want = [&](bool cond) { ok = ok && cond; };
    const bool wasWatching = g_watching.exchange(false, std::memory_order_acq_rel);
    const uint32_t identsBefore = g_identOpens.load(std::memory_order_relaxed);
    // The predicate.
    want(classify(L"C:\\Steam\\steamapps\\common\\Elite Dangerous\\Products\\"
                  L"elite-dangerous-odyssey-64\\Movies\\Ident_Frontier_EliteNeutral.webm") ==
         MoviePath::ident);
    want(classify("Movies/Ident_Frontier_Arena.webm") == MoviePath::ident);
    want(classify(L"D:\\Games\\Elite\\MOVIES\\INTRO_TEMP.WEBM") == MoviePath::ident);
    want(classify(L"\\\\?\\C:\\Games\\Elite\\Movies\\Ident_Frontier_EliteHorizons.webm") ==
         MoviePath::ident);
    want(classify(L"C:\\Games\\Elite\\Movies\\FrontEnd0.webm") == MoviePath::other);
    want(classify("Movies/SalvationFinale.webm") == MoviePath::other);
    want(classify(L"C:\\Games\\Elite\\Movies\\intro_temp.webm.bak") == MoviePath::other);
    want(classify(L"C:\\Games\\Elite\\Movies\\ident") == MoviePath::other);
    want(classify(L"C:\\Games\\Elite\\Movie\\Ident_Frontier_Arena.webm") == MoviePath::none);
    want(classify(L"C:\\Games\\Elite\\Movies\\Sub\\Ident_Frontier_Arena.webm") == MoviePath::none);
    want(classify(L"Ident_Frontier_Arena.webm") == MoviePath::none);
    want(classify(L"C:\\Games\\Elite\\Movies\\") == MoviePath::none);
    want(classify(L"") == MoviePath::none);
    want(classify(static_cast<const wchar_t*>(nullptr)) == MoviePath::none);
    want(classify(static_cast<const char*>(nullptr)) == MoviePath::none);
    want(classify(L"C:\\Games\\Elite\\Textures\\Ident_Frontier.dds") == MoviePath::none);
    if (ok) bits |= 1u;
    // The refusal, through the hook bodies, armed by hand. Nothing is
    // installed in this process's import table and nothing is forwarded:
    // an ident never reaches the original, which is what makes this safe to
    // call with no original at all.
    ok = true;
    const bool wasArmed = g_armed.exchange(true, std::memory_order_acq_rel);
    const uint32_t refusedBefore = g_refused.load(std::memory_order_relaxed);
    SetLastError(0);
    want(hookCreateFileW(L"C:\\Games\\Elite\\Movies\\Ident_Frontier_EliteNeutral.webm",
                         GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr) ==
         INVALID_HANDLE_VALUE);
    want(GetLastError() == ERROR_FILE_NOT_FOUND);
    SetLastError(0);
    want(hookCreateFileA("Movies/intro_temp.webm", GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, 0, nullptr) == INVALID_HANDLE_VALUE);
    want(GetLastError() == ERROR_FILE_NOT_FOUND);
    SetLastError(0);
    want(hookGetFileAttributesW(L"C:\\Games\\Elite\\Movies\\Ident_Frontier_Arena.webm") ==
         INVALID_FILE_ATTRIBUTES);
    want(GetLastError() == ERROR_FILE_NOT_FOUND);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    want(hookGetFileAttributesExW(L"C:\\Games\\Elite\\Movies\\Ident_Frontier_Arena.webm",
                                  GetFileExInfoStandard, &data) == FALSE);
    want(g_refused.load(std::memory_order_relaxed) == refusedBefore + 4);
    // Disarmed, the same ident is nobody's business: refuse() must say no
    // before it reaches a forward (which this process has none of), so the
    // cell asks the predicate-and-arm gate directly rather than the hook.
    g_armed.store(false, std::memory_order_release);
    want(!refuse(L"C:\\Games\\Elite\\Movies\\Ident_Frontier_Arena.webm", "cell"));
    want(g_refused.load(std::memory_order_relaxed) == refusedBefore + 4);
    if (ok) bits |= 2u;
    // The watch, disarmed: an ident is counted and NEVER refused, in either
    // width; the front end's loop is counted as "other"; a second ident
    // open (same "cell" API, so the same family's once-guard) adds to the
    // count without a second line. The refusal counter must not move.
    ok = true;
    g_identOpens.store(0, std::memory_order_relaxed);
    const uint32_t otherBefore = g_otherMovieOpens.load(std::memory_order_relaxed);
    g_watching.store(true, std::memory_order_release);
    want(!refuse(L"C:\\Games\\Elite\\Movies\\Ident_Frontier_Arena.webm", "cell"));
    want(g_identOpens.load(std::memory_order_relaxed) == 1);
    want(!refuse("Movies/intro_temp.webm", "cell"));
    want(g_identOpens.load(std::memory_order_relaxed) == 2);
    want(!refuse(L"C:\\Games\\Elite\\Movies\\FrontEnd0.webm", "cell"));
    want(g_otherMovieOpens.load(std::memory_order_relaxed) == otherBefore + 1);
    want(g_refused.load(std::memory_order_relaxed) == refusedBefore + 4);
    // Armed AND watching cannot both be true from configure, but the hook
    // must refuse if it ever were: the skip wins.
    g_armed.store(true, std::memory_order_release);
    want(refuse(L"C:\\Games\\Elite\\Movies\\Ident_Frontier_Arena.webm", "cell"));
    want(g_refused.load(std::memory_order_relaxed) == refusedBefore + 5);
    g_armed.store(false, std::memory_order_release);
    g_watching.store(false, std::memory_order_release);
    want(!refuse(L"C:\\Games\\Elite\\Movies\\Ident_Frontier_Arena.webm", "cell"));
    want(g_identOpens.load(std::memory_order_relaxed) == 2);
    if (ok) bits |= 4u;
    // Leave the session's counters as they were found.
    g_refused.store(refusedBefore, std::memory_order_relaxed);
    g_refusalLines.store(0, std::memory_order_relaxed);
    g_otherMovieOpens.store(0, std::memory_order_relaxed);
    g_identOpens.store(identsBefore, std::memory_order_relaxed);
    g_armed.store(wasArmed, std::memory_order_release);
    g_watching.store(wasWatching, std::memory_order_release);
    return bits;
}
