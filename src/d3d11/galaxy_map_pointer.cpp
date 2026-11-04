#include "galaxy_map_pointer.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <iterator>
#include <new>

#include "../common/code_hook.h"
#include "../common/game_build.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "flat_stationary_camera_capture.h"
#include "galaxy_map_pointer_core.h"
#include "galaxy_map_ui.h"

namespace edvr {

namespace {

using namespace mappointer;

// Build 334925's addresses, each resolved through src\common\game_build.h (gameRva) so an unknown build finds them by signature or the pointer stands
// down. kProjectRva is the projector's world-to-viewport function (vtable +0x18): uint64 f(void* k, const float* in, int idx, float* out). The hover
// calls it through `call rbx` and returns to kPointerReturnRva (inside GalaxyMapVisualisation); the stars' calls go through the same function from
// elsewhere. kCursorRva is the mouse cursor's per-frame update, rcx = the cursor object.
constexpr uint32_t kProjectRva = 0x28948F0;
constexpr uint32_t kPointerReturnRva = 0x3B91EE9;
constexpr uint32_t kCursorRva = 0x474A740;
constexpr uint32_t kRvas[] = {kProjectRva, kPointerReturnRva, kCursorRva};
constexpr uint8_t kProjectPrologue[10] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40};
constexpr uint8_t kCursorPrologue[12] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr uint8_t kPointerReturn[4] = {0x84, 0xC0, 0x0F, 0x84};   // at the return address: test al, al; jz rel32 (the rel32 itself is not pinned)
// The cursor object's scene object is the pointer at +0x10; its position is three floats at +0x40 of that, in the UI frame (the arrow's drawn position).
constexpr uintptr_t kCursorSceneOffset = 0x10;
constexpr uintptr_t kScenePositionOffset = 0x40;

constexpr uint64_t kLineMs = 30000;
constexpr unsigned kMaxTries = 30;

using Project = uint64_t (*)(void*, const float*, int, float*);

enum Pass : uint32_t { kNoPose, kNoCursor, kNoScene, kUnreadable, kBadArrow, kBadInput, kPassCount };
const char* const kPassNames[kPassCount] = {"no map pose", "no cursor object", "no scene object", "arrow unreadable", "arrow out of range", "input unreadable"};

struct Site {
    uint8_t* relay = nullptr;
    CodeHook hook;
    uint32_t cellAt = 0;
    uint32_t relayBytes = 0;
    std::atomic<uintptr_t>* forward = nullptr;
};

struct State {
    Site cursor, project;
    std::atomic<uintptr_t> gate{0};          // the projector relay's cell: nonzero sends the call to the detour
    std::atomic<uintptr_t> cursorObject{0};  // the cursor object the capture stub stores, one write a frame
    std::atomic<uintptr_t> forward{0};       // the projector's trampoline
    uintptr_t pointerReturn = 0;             // the pointer call's return address in this process
    uintptr_t base = 0;
    std::atomic<bool> armed{false};

    // Counters, since the last 30 s line.
    std::atomic<uint64_t> pointerCalls{0};
    std::atomic<uint64_t> replaced{0};
    std::atomic<uint64_t> passed[kPassCount] = {};

    // Frame thread only.
    unsigned tries = 0;
    uint64_t lastTryMs = 0;
    bool stood = false;
    bool open = false;
    unsigned edgeNotes = 0;
    bool openedInWindow = false;
    uint64_t lastLineMs = 0;
    bool cursorSaid = false;
    const char* why = "";
};

State* g_s = nullptr;

bool readMem(uintptr_t at, void* out, size_t n) noexcept {
    __try { std::memcpy(out, reinterpret_cast<const void*>(at), n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- the hooks ---------------------------------------------------------------------------------------------------------------------------------------

uint8_t* allocateRelay(uintptr_t target) noexcept {
    constexpr uintptr_t granularity = 64 * 1024, span = 0x7FFF0000ull;
    const uintptr_t begin = (target - span) & ~(granularity - 1);
    for (uintptr_t candidate = target; candidate >= begin; candidate -= granularity) {
        auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (p) return p;
    }
    return nullptr;
}

bool prepareSite(void* trampoline, void* opaque) noexcept {
    Site& site = *static_cast<Site*>(opaque);
    const uint64_t original = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(site.relay + site.cellAt, &original, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(site.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) || !FlushInstructionCache(GetCurrentProcess(), site.relay, site.relayBytes)) return false;
    if (site.forward) site.forward->store(static_cast<uintptr_t>(original), std::memory_order_release);
    return true;
}

// The pointer's near point is replaced by a far one along the arrow's direction. Counted by outcome; every pass-through calls the original with the
// game's own input.
uint64_t pass(State& s, Project original, Pass why, void* k, const float* in, int idx, float* out) noexcept {
    s.passed[why].fetch_add(1, std::memory_order_relaxed);
    return original(k, in, idx, out);
}

uint64_t pointerCall(State& s, Project original, void* k, const float* in, int idx, float* out) noexcept {
    s.pointerCalls.fetch_add(1, std::memory_order_relaxed);
    MapCameraPose pose;
    if (!mapCameraPose(pose)) return pass(s, original, kNoPose, k, in, idx, out);
    const uintptr_t cursor = s.cursorObject.load(std::memory_order_acquire);
    if (!cursor) return pass(s, original, kNoCursor, k, in, idx, out);
    uintptr_t scene = 0;
    if (!readMem(cursor + kCursorSceneOffset, &scene, sizeof(scene))) return pass(s, original, kUnreadable, k, in, idx, out);
    if (!scene) return pass(s, original, kNoScene, k, in, idx, out);
    float arrow[3];
    if (!readMem(scene + kScenePositionOffset, arrow, sizeof(arrow))) return pass(s, original, kUnreadable, k, in, idx, out);
    float game[4];
    if (!readMem(reinterpret_cast<uintptr_t>(in), game, sizeof(game))) return pass(s, original, kBadInput, k, in, idx, out);
    alignas(16) float farIn[4];
    if (farPoint(pose.rows, pose.position, arrow, game[3], farIn) != Why::kOk) return pass(s, original, kBadArrow, k, in, idx, out);
    const uint64_t ret = original(k, farIn, idx, out);
    s.replaced.fetch_add(1, std::memory_order_relaxed);
    return ret;
}

// Reached through the relay's jump, so _ReturnAddress() is the game's caller.
__declspec(noinline) uint64_t projectDetour(void* k, const float* in, int idx, float* out) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    State& s = *g_s;
    const auto original = reinterpret_cast<Project>(s.forward.load(std::memory_order_acquire));
    if (caller != s.pointerReturn) return original(k, in, idx, out);   // the stars' own calls and any other: the game's, untouched
    return pointerCall(s, original, k, in, idx, out);
}

// Allocates one hook's relay within reach of its target. A non-null reason on failure.
const char* installSite(Site& site, uintptr_t target, uint32_t relayBytes, uint32_t cellAt) {
    site.relay = allocateRelay(target);
    if (!site.relay) return "relay-allocation-failed";
    site.relayBytes = relayBytes;
    site.cellAt = cellAt;
    return nullptr;
}


// One try. True when both hooks are in. On a failure s.why names it and the return says whether another try could help.
bool tryInstall(State& s, bool& transient) {
    transient = false;
    const HMODULE game = GetModuleHandleW(L"EliteDangerous64.exe");
    if (!game) { s.why = "game-module-unavailable"; transient = true; return false; }
    const uintptr_t base = reinterpret_cast<uintptr_t>(game);
    uint32_t missing = 0;
    if (!gameRvasFound(kRvas, std::size(kRvas), &missing)) {
        s.why = "not-found-by-signature";
        transient = true;   // an unknown build's signatures resolve in the background
        return false;
    }
    const uintptr_t projectAt = base + gameRva(kProjectRva);
    const uintptr_t cursorAt = base + gameRva(kCursorRva);
    const uintptr_t returnAt = base + gameRva(kPointerReturnRva);
    if (!gameRva(kProjectRva) || !gameRva(kCursorRva) || !gameRva(kPointerReturnRva)) { s.why = "not-found-by-signature"; return false; }
    uint8_t projectFound[sizeof(kProjectPrologue)] = {}, cursorFound[sizeof(kCursorPrologue)] = {}, returnFound[sizeof(kPointerReturn)] = {};
    if (!readMem(projectAt, projectFound, sizeof(projectFound)) || !readMem(cursorAt, cursorFound, sizeof(cursorFound)) || !readMem(returnAt, returnFound, sizeof(returnFound)) ||
        !gamePinMatches(kProjectRva, projectFound, kProjectPrologue, sizeof(kProjectPrologue)) || !gamePinMatches(kCursorRva, cursorFound, kCursorPrologue, sizeof(kCursorPrologue)) ||
        !gamePinMatches(kPointerReturnRva, returnFound, kPointerReturn, sizeof(kPointerReturn))) {
        s.why = "prologue-or-return-site-mismatch";
        return false;
    }
    s.base = base;
    s.pointerReturn = returnAt;
    if (!s.cursor.hook.installed()) {
        if (const char* r = installSite(s.cursor, cursorAt, kCaptureStubBytes, kCaptureStubTrampolineAt)) { s.why = r; return false; }
        buildCaptureStub(s.cursor.relay, &s.cursorObject);
        if (!s.cursor.hook.install(reinterpret_cast<void*>(cursorAt), s.cursor.relay, nullptr, "galaxy-map-pointer-cursor", &prepareSite, &s.cursor)) {
            s.why = s.cursor.hook.refusal();
            VirtualFree(s.cursor.relay, 0, MEM_RELEASE);
            s.cursor.relay = nullptr;
            return false;
        }
    }
    if (!s.project.hook.installed()) {
        s.project.forward = &s.forward;
        if (const char* r = installSite(s.project, projectAt, kGateRelayBytes, kGateRelayTrampolineAt)) { s.why = r; return false; }
        buildGateRelay(s.project.relay, &s.gate, reinterpret_cast<const void*>(&projectDetour));
        if (!s.project.hook.install(reinterpret_cast<void*>(projectAt), s.project.relay, nullptr, "galaxy-map-pointer-projector", &prepareSite, &s.project)) {
            s.why = s.project.hook.refusal();
            VirtualFree(s.project.relay, 0, MEM_RELEASE);
            s.project.relay = nullptr;
            return false;
        }
    }
    return true;
}

void closeGate(State& s) noexcept {
    s.gate.store(0, std::memory_order_release);
    mapui::setGates(false, false);
    s.open = false;
}

void thirtySecondLine(State& s, uint64_t now) {
    using namespace mapui;
    uint64_t swapped[kSwapCount], swappedTotal = 0, maskIssued[3], maskTotal = 0;
    for (uint32_t i = 0; i < kSwapCount; ++i) { swapped[i] = g_counters.swapped[i].exchange(0, std::memory_order_relaxed); swappedTotal += swapped[i]; }
    for (int i = 0; i < 3; ++i) { maskIssued[i] = g_counters.maskIssued[i].exchange(0, std::memory_order_relaxed); maskTotal += maskIssued[i]; }
    const uint64_t arrowKept = g_counters.arrowKept.exchange(0, std::memory_order_relaxed), maskLate = g_counters.maskLate.exchange(0, std::memory_order_relaxed);
    const uint64_t probes = g_counters.probes.exchange(0, std::memory_order_relaxed), answered = g_counters.answered.exchange(0, std::memory_order_relaxed);
    const uint64_t framesOver = g_counters.framesOver.exchange(0, std::memory_order_relaxed), framesClear = g_counters.framesClear.exchange(0, std::memory_order_relaxed);
    const uint64_t ringFull = g_counters.ringFull.exchange(0, std::memory_order_relaxed), perFrameFull = g_counters.perFrameFull.exchange(0, std::memory_order_relaxed);
    const uint64_t probeFaults = g_counters.probeFaults.exchange(0, std::memory_order_relaxed), unavailable = g_counters.maskUnavailable.exchange(0, std::memory_order_relaxed);
    const uint64_t calls = s.pointerCalls.exchange(0, std::memory_order_relaxed);
    const uint64_t replaced = s.replaced.exchange(0, std::memory_order_relaxed);
    uint64_t passed[kPassCount], passedTotal = 0;
    for (uint32_t i = 0; i < kPassCount; ++i) { passed[i] = s.passed[i].exchange(0, std::memory_order_relaxed); passedTotal += passed[i]; }
    const bool wasOpen = s.openedInWindow;
    s.openedInWindow = s.open;
    s.lastLineMs = now;
    const uint64_t declinesTotal = ringFull + perFrameFull + probeFaults + unavailable + maskLate;
    if (!wasOpen && !calls && !swappedTotal && !maskTotal && !probes && !answered && !declinesTotal) return;
    char reasons[256] = "";
    size_t n = 0;
    for (uint32_t i = 0; i < kPassCount && n + 40 < sizeof(reasons); ++i)
        if (passed[i]) n += static_cast<size_t>(std::snprintf(reasons + n, sizeof(reasons) - n, "%s%s %llu", n ? ", " : "", kPassNames[i], static_cast<unsigned long long>(passed[i])));
    Log::get().note("map pointer: Galaxy Map %s over the last 30 s; hover: the pointer's projector call ran %llu times, %llu replaced by the arrow's far point, %llu passed through%s%s%s; "
                    "cursor object %s; draws swapped: icons C31238 %llu, labels 12382D %llu, arrow 4EF6DD %llu (arrow left the game's, tip on a panel or no answer: %llu); "
                    "panel draws issued into the mask: C4B4B3 %llu, B10B03 %llu, 5DAD41 %llu; probes made %llu, answered %llu (frames over a panel %llu, not over %llu); "
                    "tip on a panel now: %s; declines: %llu (ring full %llu, eye-frame full %llu, faulted %llu, no mask or query %llu, panel draws after the eye's probe %llu)",
                    s.open ? "open now" : wasOpen ? "was open" : "not open", static_cast<unsigned long long>(calls), static_cast<unsigned long long>(replaced),
                    static_cast<unsigned long long>(passedTotal), passedTotal ? " (" : "", reasons, passedTotal ? ")" : "",
                    s.cursorObject.load(std::memory_order_acquire) ? "seen" : "never seen", static_cast<unsigned long long>(swapped[kSwapIconA]),
                    static_cast<unsigned long long>(swapped[kSwapIconB]), static_cast<unsigned long long>(swapped[kSwapArrow]), static_cast<unsigned long long>(arrowKept),
                    static_cast<unsigned long long>(maskIssued[0]), static_cast<unsigned long long>(maskIssued[1]), static_cast<unsigned long long>(maskIssued[2]),
                    static_cast<unsigned long long>(probes), static_cast<unsigned long long>(answered), static_cast<unsigned long long>(framesOver),
                    static_cast<unsigned long long>(framesClear), over() ? "yes" : "no", static_cast<unsigned long long>(declinesTotal), static_cast<unsigned long long>(ringFull),
                    static_cast<unsigned long long>(perFrameFull), static_cast<unsigned long long>(probeFaults), static_cast<unsigned long long>(unavailable),
                    static_cast<unsigned long long>(maskLate));
}

} // namespace

bool mapPointerArmed() noexcept {
    const State* s = g_s;
    return s && s->armed.load(std::memory_order_acquire);
}

void mapPointerFrame() noexcept {
    if (runtimeFlatProfile()) {
        // A live switch to flat: the gate closes and the draws pay nothing. Switching back reopens it with the map.
        if (g_s) closeGate(*g_s);
        return;
    }
    if (!g_s) {
        State* fresh = new (std::nothrow) State;   // never freed: the hooks outlive every owner
        if (!fresh) {
            static bool noted = false;
            if (!noted) { noted = true; Log::get().note("map pointer: off: its storage could not be allocated"); }
            return;
        }
        g_s = fresh;
    }
    State& s = *g_s;
    const uint64_t now = GetTickCount64();
    const bool open = mapCameraGalaxyOpen();
    if (!s.armed.load(std::memory_order_acquire)) {
        mapui::setGates(open, false);   // the icons face the eye whether or not the pointer's hooks can be had
        if (s.stood) return;
        if (s.tries && now - s.lastTryMs < 1000) return;
        s.lastTryMs = now;
        ++s.tries;
        bool transient = false;
        if (!tryInstall(s, transient)) {
            if (!transient || s.tries >= kMaxTries) {
                s.stood = true;
                Log::get().note("map pointer: stands down after %u tries (%s): the hover, the arrow's depth and the panel mask stay as the game made them (the icons still face the eye)%s", s.tries, s.why,
                                s.cursor.hook.installed() ? "; the cursor capture stays in as a harmless store of one register" : "");
            }
            return;
        }
        s.armed.store(true, std::memory_order_release);
        s.lastLineMs = now;
        Log::get().note("map pointer: armed (VR profile): the cursor object capture is in at +0x%llX and the projector relay at +0x%llX, gate closed until the Galaxy Map opens; "
                        "the pointer's call (the only one replaced) returns to +0x%llX",
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s.cursor.hook.target()) - s.base),
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s.project.hook.target()) - s.base), static_cast<unsigned long long>(s.pointerReturn - s.base));
    }
    if (open != s.open) {
        if (open) s.openedInWindow = true;
        s.open = open;
        if (s.edgeNotes < 16) {
            ++s.edgeNotes;
            Log::get().note("map pointer: the Galaxy Map %s: the hover reads the arrow's direction, the icons and labels face the eye, the arrow is at star depth off the panels and the game's on them, and the panel mask is %s",
                            open ? "opened" : "closed", open ? "kept" : "let go");
        }
    }
    s.gate.store(open ? 1 : 0, std::memory_order_release);
    mapui::setGates(open, open);
    if (!s.cursorSaid && s.cursorObject.load(std::memory_order_acquire)) {
        s.cursorSaid = true;
        Log::get().note("map pointer: the cursor update ran: its object is at %p", reinterpret_cast<void*>(s.cursorObject.load(std::memory_order_acquire)));
    }
    if (now - s.lastLineMs >= kLineMs) thirtySecondLine(s, now);
}

} // namespace edvr
