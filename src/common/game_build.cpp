// The game-build resolver's Windows half (game_build.h): the running image read under SEH, the once-only resolve and its log line,
// and the temporary update check (advanced.game_update_check). The matcher and the line texts are game_build_core.h's, held to
// tools\hook_signatures.py by tools\game_build_test.
#include "game_build.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "config.h"
#include "game_build_core.h"
#include "log.h"

namespace edvr {
namespace {

namespace sig {
using namespace gamesig;
#include "game_signatures.inc"
}  // namespace sig

static_assert(sig::kTable.format == gamesig::kCppFormat, "game_signatures.inc was emitted for another layout of game_build_core.h");
constexpr uint32_t kTargetCount = sig::kTable.targetCount;
constexpr uint32_t kTargetSlots = kTargetCount ? kTargetCount : 1;

// kNotGame: the PE header reads, the identity is not the known build's, and the main module is not EliteDangerous64.exe.
enum : int { kUnread = 0, kKnown, kUnknown, kUnreadable, kNotGame };
std::atomic<int> g_identity{kUnread};
std::mutex g_identityMutex;
uint32_t g_stamp = 0, g_size = 0;   // written before g_identity is published
char g_text[160] = "";

std::mutex g_resolveMutex;
std::atomic<bool> g_resolved{false};
gamesig::Result g_results[kTargetSlots];
gamesig::RvaIndex g_index;   // gameRva's lookup over g_results, built before g_resolved is published
// The "game build:" line of a resolve that ran before the log opened (a hook asked before gameBuildStartup); gameBuildStartup writes it
// once the log is open. Under g_resolveMutex.
std::string g_pendingSummary;

// The main module's file name (its last path component) as UTF-8 into out, and whether it is the game's executable. A name that does not
// fit the buffer is not the game's.
bool mainModuleIsGame(char* out, size_t cap) noexcept {
    wchar_t path[1024] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    const bool whole = n > 0 && n < sizeof(path) / sizeof(path[0]);
    if (!whole) path[0] = L'\0';
    const wchar_t* leaf = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') leaf = p + 1;
    if (cap) {
        const int w = WideCharToMultiByte(CP_UTF8, 0, leaf, -1, out, static_cast<int>(cap), nullptr, nullptr);
        if (w <= 0) std::snprintf(out, cap, "%s", whole ? "(a name that does not convert)" : "(no module name)");
        out[cap - 1] = '\0';
    }
    return whole && gamesig::isGameExecutable(path);
}

// base+0x3C -> e_lfanew, then the TimeDateStamp at +8 and the SizeOfImage at +0x50 (the pair every build-keyed hook checks). Under
// __try: an unreadable header is a refusal, not a crash.
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

int identity() noexcept {
    const int s = g_identity.load(std::memory_order_acquire);
    if (s != kUnread) return s;
    std::lock_guard<std::mutex> lock(g_identityMutex);
    if (g_identity.load(std::memory_order_relaxed) != kUnread) return g_identity.load(std::memory_order_relaxed);
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uint32_t stamp = 0, size = 0;
    int next = kUnreadable;
    char name[96] = "";
    if (base && readIdentity(base, &stamp, &size)) {
        // The known build's identity is the game's whatever its file is called, so that path does not change. Any other identity is a
        // build of the game only when the main module is EliteDangerous64.exe: a stranger's executable is never resolved.
        if (stamp == sig::kKnownStamp && size == sig::kKnownImageSize)
            next = kKnown;
        else
            next = mainModuleIsGame(name, sizeof(name)) ? kUnknown : kNotGame;
    }
    g_stamp = next == kUnreadable ? 0 : stamp;
    g_size = next == kUnreadable ? 0 : size;
    if (next == kKnown)
        std::snprintf(g_text, sizeof(g_text), "build %s", sig::kKnownBuild);
    else if (next == kUnknown)
        std::snprintf(g_text, sizeof(g_text), "unknown build (PE stamp 0x%08X, image size 0x%X)", static_cast<unsigned>(stamp),
                      static_cast<unsigned>(size));
    else if (next == kNotGame)
        std::snprintf(g_text, sizeof(g_text), "not the game (the process is %s)", name);
    else
        std::snprintf(g_text, sizeof(g_text), "unreadable build (no PE header)");
    g_identity.store(next, std::memory_order_release);
    return next;
}

bool sehCopy(const void* ctx, uint32_t rva, void* dst, size_t n) noexcept {
    __try {
        std::memcpy(dst, static_cast<const uint8_t*>(ctx) + rva, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Resolves the table against the running executable into out; the wall time in *ms.
bool resolveRunning(gamesig::Result* out, gamesig::Stats* stats, double* ms) noexcept {
    const HMODULE exe = GetModuleHandleW(nullptr);
    LARGE_INTEGER freq{}, a{}, b{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&a);
    const gamesig::ImageView img{exe, &sehCopy, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(exe))};
    const bool ok = exe && gamesig::resolveTable(sig::kTable, img, out, stats);
    QueryPerformanceCounter(&b);
    *ms = freq.QuadPart ? double(b.QuadPart - a.QuadPart) * 1000.0 / double(freq.QuadPart) : 0.0;
    return ok;
}

void ensureResolved() noexcept {
    if (g_resolved.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_resolveMutex);
    if (g_resolved.load(std::memory_order_relaxed)) return;
    gamesig::Stats stats{};
    double ms = 0.0;
    const bool ok = resolveRunning(g_results, &stats, &ms);
    try {
        g_index.build(sig::kTable, g_results);
    } catch (...) {
        // Out of memory: gameRva falls back to unitRva's scan (g_index.built() is false).
    }
    try {
        const std::string summary = gamesig::resolveSummaryText(sig::kTable, g_results, stats, ok, ms, gameBuildText());
        if (Log::get().isOpen())
            Log::get().note("game build: %s", summary.c_str());
        else
            g_pendingSummary = summary;
    } catch (...) {
    }
    g_resolved.store(true, std::memory_order_release);
}

// Writes the line of a resolve that finished before the log opened.
void notePendingSummary() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_resolveMutex);
        if (g_pendingSummary.empty()) return;
        Log::get().note("game build: %s (resolved before the log opened)", g_pendingSummary.c_str());
        g_pendingSummary.clear();
    } catch (...) {
    }
}

uint8_t firstByte(uintptr_t at) noexcept {
    __try {
        return *reinterpret_cast<const volatile uint8_t*>(at);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The update check (advanced.game_update_check): the resolver against the running known build, into its own results, on this
// background thread. The hooks never read these.
gamesig::Result g_checkResults[kTargetSlots];

void runUpdateCheck() noexcept {
    gamesig::Stats stats{};
    double ms = 0.0;
    const bool ok = resolveRunning(g_checkResults, &stats, &ms);
    // A hook that installed before the check read its unit has written a jump there, and that unit no longer matches.
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uint32_t jumps = 0;
    for (uint32_t i = 0; ok && base && i < kTargetCount; ++i)
        if (g_checkResults[i].reason != gamesig::kFound && sig::kTable.targets[i].kind == gamesig::kCode &&
            firstByte(base + sig::kTable.targets[i].rva) == 0xE9)
            ++jumps;
    try {
        Log::get().note("game update check: %s", gamesig::updateCheckText(sig::kTable, g_checkResults, stats, ok, ms, jumps).c_str());
    } catch (...) {
    }
}

}  // namespace

bool gameBuildIdentity(uint32_t* stamp, uint32_t* size) noexcept {
    const int s = identity();
    if (stamp) *stamp = g_stamp;
    if (size) *size = g_size;
    return s != kUnreadable;
}

bool gameBuildKnown() noexcept { return identity() == kKnown; }

bool gameBuildUnknown() noexcept { return identity() == kUnknown; }

const char* gameBuildText() noexcept {
    identity();
    return g_text;
}

uintptr_t gameRva(uint32_t knownRva) noexcept {
    const int s = identity();
    if (s == kKnown) return knownRva;
    if (s != kUnknown) return 0;
    ensureResolved();
    return g_index.built() ? g_index.rva(knownRva) : gamesig::unitRva(sig::kTable, g_results, knownRva);
}

bool gameRvasFound(const uint32_t* knownRvas, size_t n, uint32_t* firstMissing) noexcept {
    if (firstMissing) *firstMissing = 0;
    for (size_t i = 0; knownRvas && i < n; ++i)
        if (gameRva(knownRvas[i]) == 0) {
            if (firstMissing) *firstMissing = knownRvas[i];
            return false;
        }
    return knownRvas != nullptr || n == 0;
}

bool gamePinMatches(uint32_t knownRva, const uint8_t* got, const uint8_t* pin, size_t n) noexcept {
    if (!got || !pin) return false;
    const int s = identity();
    if (s == kKnown) return std::memcmp(got, pin, n) == 0;
    if (s != kUnknown) return false;
    ensureResolved();
    return gamesig::maskedRangeEqual(sig::kTable, g_results, knownRva, got, pin, n);
}

bool gamePinProvenByMatch(uint32_t knownRva, size_t n) noexcept {
    if (identity() != kUnknown) return false;
    ensureResolved();
    return gamesig::coverRange(sig::kTable, g_results, knownRva, n);
}

void gameBuildStartup(void (*afterResolve)()) noexcept {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    // An unknown build: resolve now, on a thread of its own, so no hook pays the resolve on the thread that first asks (the render thread,
    // for some), and nothing pays it in DllMain under the loader lock (the VR Supersampling hold defers its install to afterResolve on
    // an unknown build). A hook that asks before it is done waits on the same lock. The known build starts nothing here.
    if (identity() == kUnknown) {
        notePendingSummary();
        try {
            std::thread([afterResolve] {
                ensureResolved();
                if (afterResolve) afterResolve();
            }).detach();
        } catch (...) {
            Log::get().note("game build: %s: could not start the resolver's thread; the first hook that needs a game address resolves "
                            "them on its own thread.", gameBuildText());
            if (afterResolve) afterResolve();
        }
    } else if (identity() == kNotGame) {
        Log::get().note("game build: %s, not EliteDangerous64.exe: no game address is resolved, and every fix that hooks the game "
                        "stands down.", gameBuildText());
    }
    try {
        // TEMPORARY (issue 124, docs\design-game-update-signatures-2026-10-25.md): remove with the arc.
        const std::string value = Config::get().getString("advanced.game_update_check", "off");
        if (_stricmp(value.c_str(), "on") != 0) return;
        if (identity() != kKnown) {
            Log::get().note("game update check: on, but this is %s; the check runs only on build %s (here the hooks find their "
                            "addresses by signature, and the \"game build:\" line says how many).", gameBuildText(), sig::kKnownBuild);
            return;
        }
        Log::get().note("game update check: on; the signature resolver runs once against the running %s on a background thread "
                        "and reports whether every address it finds is the one the hooks use. The hooks are not changed.",
                        gameBuildText());
        std::thread(&runUpdateCheck).detach();
    } catch (...) {
        Log::get().note("game update check: could not start its thread; nothing was checked.");
    }
}

}  // namespace edvr
