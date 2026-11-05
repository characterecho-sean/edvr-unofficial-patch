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
// elsewhere. kCursorRva is the mouse cursor's per-frame update, rcx = the cursor object. kSystemPointerReturnRva is the same call in the System Map's per-frame step (docs/build-334925.md):
// the same K and the same pointer block's +0x30, and its answer goes to the map's hover and select, which compare it in viewport x, y with each body projected through K. Optional: a build
// where it is not found keeps the Galaxy Map's pointer, and the System Map stays as the game made it.
constexpr uint32_t kProjectRva = 0x28948F0;
constexpr uint32_t kPointerReturnRva = 0x3B91EE9;
constexpr uint32_t kSystemPointerReturnRva = 0x3AF92A7;
constexpr uint32_t kCursorRva = 0x474A740;
constexpr uint32_t kRvas[] = {kProjectRva, kPointerReturnRva, kCursorRva};
constexpr uint32_t kSystemRvas[] = {kSystemPointerReturnRva};
constexpr uint8_t kProjectPrologue[10] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40};
constexpr uint8_t kCursorPrologue[12] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr uint8_t kPointerReturn[4] = {0x84, 0xC0, 0x0F, 0x84};   // at the return address: test al, al; jz rel32 (the rel32 itself is not pinned)
constexpr uint8_t kSystemPointerReturn[12] = {0x48, 0x8B, 0x7C, 0x24, 0x70, 0x48, 0x8B, 0x8E, 0x10, 0x04, 0x00, 0x00};   // at the return address: mov rdi, [rsp+0x70]; mov rcx, [rsi+0x410] (the map's store)
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
    uintptr_t systemReturn = 0;              // the System Map's, 0 when this build's was not found
    uintptr_t base = 0;
    std::atomic<bool> armed{false};

    // Counters, since the last 30 s line.
    std::atomic<uint64_t> pointerCalls{0};
    std::atomic<uint64_t> replaced{0};
    std::atomic<uint64_t> heldCalls{0};
    std::atomic<uint64_t> passed[kPassCount] = {};
    PointerHold held;                        // the System Map's held answer: the game's update thread only

    // Frame thread only.
    unsigned tries = 0;
    uint64_t lastTryMs = 0;
    bool stood = false;
    bool open = false;
    unsigned edgeNotes = 0;
    bool openedInWindow = false;
    uint64_t openFrames = 0;   // frames with the map open since the last 30 s line
    uint64_t galaxyFrames = 0; // of those, the Galaxy Map's: the run of sprites is its alone
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

uint64_t pointerCall(State& s, Project original, void* k, const float* in, int idx, float* out, bool hold) noexcept {
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
    s.replaced.fetch_add(1, std::memory_order_relaxed);
    if (!hold) return original(k, farIn, idx, out);
    // The System Map's click wants the same bits at press and release (galaxy_map_pointer_core.h, THE HELD POINTER). Its call passes a 16-byte out.
    const uint64_t changes = mapCameraChanges();
    if (holdApplies(s.held, arrow, changes)) {
        std::memcpy(out, s.held.out, sizeof(s.held.out));
        s.heldCalls.fetch_add(1, std::memory_order_relaxed);
        return s.held.ret;
    }
    const uint64_t ret = original(k, farIn, idx, out);
    holdTake(s.held, arrow, changes, out, ret);
    return ret;
}

// Reached through the relay's jump, so _ReturnAddress() is the game's caller.
__declspec(noinline) uint64_t projectDetour(void* k, const float* in, int idx, float* out) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    State& s = *g_s;
    const auto original = reinterpret_cast<Project>(s.forward.load(std::memory_order_acquire));
    if (caller != s.pointerReturn && caller != s.systemReturn) return original(k, in, idx, out);   // the stars' and bodies' own calls and any other: the game's, untouched
    return pointerCall(s, original, k, in, idx, out, caller == s.systemReturn);
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
    {
        // Optional, and written before the projector's hook goes in: without it the System Map stays the game's.
        uint8_t systemFound[sizeof(kSystemPointerReturn)] = {};
        const uint32_t systemRva = gameRvasFound(kSystemRvas, std::size(kSystemRvas), &missing) ? gameRva(kSystemPointerReturnRva) : 0;
        s.systemReturn = systemRva && readMem(base + systemRva, systemFound, sizeof(systemFound)) && gamePinMatches(kSystemPointerReturnRva, systemFound, kSystemPointerReturn, sizeof(kSystemPointerReturn)) ? base + systemRva : 0;
    }
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
    mapui::setSystemOpen(false);
    mapui::setGates(false, false);
    s.open = false;
}

void thirtySecondLine(State& s, uint64_t now) {
    using namespace mapui;
    uint64_t swapped[kSwapCount], swappedTotal = 0, maskIssued[2], maskTotal = 0;
    for (uint32_t i = 0; i < kSwapCount; ++i) { swapped[i] = g_counters.swapped[i].exchange(0, std::memory_order_relaxed); swappedTotal += swapped[i]; }
    for (int i = 0; i < 2; ++i) { maskIssued[i] = g_counters.maskIssued[i].exchange(0, std::memory_order_relaxed); maskTotal += maskIssued[i]; }
    const uint64_t noPose = g_counters.noPose.exchange(0, std::memory_order_relaxed), camWrites = g_counters.camWrites.exchange(0, std::memory_order_relaxed);
    const uint64_t arrowKept = g_counters.arrowKept.exchange(0, std::memory_order_relaxed), maskLate = g_counters.maskLate.exchange(0, std::memory_order_relaxed);
    const uint64_t probes = g_counters.probes.exchange(0, std::memory_order_relaxed), answered = g_counters.answered.exchange(0, std::memory_order_relaxed);
    const uint64_t stickyProbes = g_counters.stickyProbes.exchange(0, std::memory_order_relaxed);
    const uint64_t framesOver = g_counters.framesOver.exchange(0, std::memory_order_relaxed), framesClear = g_counters.framesClear.exchange(0, std::memory_order_relaxed);
    const uint64_t ringFull = g_counters.ringFull.exchange(0, std::memory_order_relaxed), perFrameFull = g_counters.perFrameFull.exchange(0, std::memory_order_relaxed);
    const uint64_t probeFaults = g_counters.probeFaults.exchange(0, std::memory_order_relaxed), unavailable = g_counters.maskUnavailable.exchange(0, std::memory_order_relaxed);
    const uint64_t depthMade = g_depth.made.exchange(0, std::memory_order_relaxed), depthUnavailable = g_depth.unavailable.exchange(0, std::memory_order_relaxed);
    const uint64_t depthTested = g_depth.tested.exchange(0, std::memory_order_relaxed), depthIssued = g_depth.issued.exchange(0, std::memory_order_relaxed);
    const uint64_t depthFaults = g_depth.faults.exchange(0, std::memory_order_relaxed), writeBackDeclined = g_writeBackDeclined.exchange(0, std::memory_order_relaxed);
    const uint64_t eyesValid = g_counters.eyesValid.exchange(0, std::memory_order_relaxed), eyeGenSteps = g_counters.eyeGenSteps.exchange(0, std::memory_order_relaxed);
    const uint64_t calls = s.pointerCalls.exchange(0, std::memory_order_relaxed);
    const uint64_t replaced = s.replaced.exchange(0, std::memory_order_relaxed);
    uint64_t passed[kPassCount], passedTotal = 0;
    for (uint32_t i = 0; i < kPassCount; ++i) { passed[i] = s.passed[i].exchange(0, std::memory_order_relaxed); passedTotal += passed[i]; }
    const bool wasOpen = s.openedInWindow;
    s.openedInWindow = s.open;
    s.lastLineMs = now;
    // The run of sprites (map_run_core.h): the render target binds the map's sprites cost. Said whenever the map was open in the window, even with no run: the line then says 0 runs.
    const uint64_t runBinds = g_run.binds.exchange(0, std::memory_order_relaxed), runsStarted = g_run.started.exchange(0, std::memory_order_relaxed);
    const uint64_t runsEnded = g_run.ended.exchange(0, std::memory_order_relaxed), setsKept = g_run.setsKept.exchange(0, std::memory_order_relaxed);
    const uint64_t setsDropped = g_run.setsDropped.exchange(0, std::memory_order_relaxed), setsOwn = g_run.setsOwn.exchange(0, std::memory_order_relaxed), frames = s.openFrames;
    s.openFrames = 0;
    const uint64_t galaxyFrames = s.galaxyFrames;
    s.galaxyFrames = 0;
    if (galaxyFrames || runBinds || runsStarted)
        Log::get().note("map pointer: render target binds for the map's sprites %.1f a frame (%llu in all over %llu map frames); runs of sprites started %.2f a frame (%llu), ended %.2f a frame (%llu); "
                        "the game's own sets of the saved targets inside a run kept %llu, of others %llu, of the layer's own read back %llu%s",
                        frames ? static_cast<double>(runBinds) / static_cast<double>(frames) : 0.0, static_cast<unsigned long long>(runBinds), static_cast<unsigned long long>(frames),
                        frames ? static_cast<double>(runsStarted) / static_cast<double>(frames) : 0.0, static_cast<unsigned long long>(runsStarted),
                        frames ? static_cast<double>(runsEnded) / static_cast<double>(frames) : 0.0, static_cast<unsigned long long>(runsEnded),
                        static_cast<unsigned long long>(setsKept), static_cast<unsigned long long>(setsDropped), static_cast<unsigned long long>(setsOwn),
                        runsStarted ? "" : (frames ? " -- 0 runs: the sprites were not drawn in a run" : ""));
    const uint64_t declinesTotal = ringFull + perFrameFull + probeFaults + unavailable + maskLate;
    if (!wasOpen && !calls && !swappedTotal && !maskTotal && !probes && !answered && !declinesTotal && !depthMade && !depthUnavailable && !depthTested && !depthIssued && !depthFaults && !writeBackDeclined && !eyesValid) return;
    char reasons[256] = "";
    size_t n = 0;
    for (uint32_t i = 0; i < kPassCount && n + 40 < sizeof(reasons); ++i)
        if (passed[i]) n += static_cast<size_t>(std::snprintf(reasons + n, sizeof(reasons) - n, "%s%s %llu", n ? ", " : "", kPassNames[i], static_cast<unsigned long long>(passed[i])));
    Log::get().note("map pointer: a map %s over the last 30 s; hover: the pointer's projector call ran %llu times, %llu replaced by the arrow's far point (the System Map's held and answered unchanged %llu), %llu passed through%s%s%s; "
                    "cursor object %s; the System Map's markers scaled %llu (%u bodies known), arrow's depth %.2f units (0: at infinity), cut-outs behind its bodies %llu, reticule lifted %llu; draws swapped: icons C31238 %llu, labels 12382D %llu, arrow 4EF6DD %llu (arrow left the game's, tip on a panel or no answer: %llu), rings 94EA99 %llu, stems 3028E7 %llu (icons, labels, rings and stems left the game's for want of the map camera's pose: %llu; its constants written %llu times, %llu of them with the two eyes' offsets, which changed %llu times%s); "
                    "panel draws issued into the mask: C4B4B3 %llu, 5DAD41 %llu; probes made %llu (%llu with the sticky edge), answered %llu (frames over a panel %llu, not over %llu);"
                    "tip on a panel now: %s; declines: %llu (ring full %llu, eye-frame full %llu, faulted %llu, no mask or query %llu, panel draws after the eye's probe %llu); "
                    "sprite depth: textures made %llu, sprite draws drawn with the depth tested %llu, depth issues made %llu, faulted %llu, not made (a texture, state or shader that could not be made) %llu, stencil write-backs of the map's sprites and canvases declined (off while the map is open in VR) %llu%s",
                    s.open ? "open now" : wasOpen ? "was open" : "not open", static_cast<unsigned long long>(calls), static_cast<unsigned long long>(replaced),
                    static_cast<unsigned long long>(s.heldCalls.exchange(0, std::memory_order_relaxed)),
                    static_cast<unsigned long long>(passedTotal), passedTotal ? " (" : "", reasons, passedTotal ? ")" : "",
                    s.cursorObject.load(std::memory_order_acquire) ? "seen" : "never seen", static_cast<unsigned long long>(swapped[kSwapMarker]), bodiesKnown(), static_cast<double>(arrowDepth()),
                    static_cast<unsigned long long>(g_bodyCutouts.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(swapped[kSwapReticule]), static_cast<unsigned long long>(swapped[kSwapIconA]),
                    static_cast<unsigned long long>(swapped[kSwapIconB]), static_cast<unsigned long long>(swapped[kSwapArrow]), static_cast<unsigned long long>(arrowKept),
                    static_cast<unsigned long long>(swapped[kSwapRing]), static_cast<unsigned long long>(swapped[kSwapStem]), static_cast<unsigned long long>(noPose),
                    static_cast<unsigned long long>(camWrites), static_cast<unsigned long long>(eyesValid), static_cast<unsigned long long>(eyeGenSteps),
                    camWrites && !eyesValid ? " -- NO write carried the eyes: the overlays use the game's rounded eye" : "",
                    static_cast<unsigned long long>(maskIssued[0]), static_cast<unsigned long long>(maskIssued[1]),
                    static_cast<unsigned long long>(probes), static_cast<unsigned long long>(stickyProbes), static_cast<unsigned long long>(answered), static_cast<unsigned long long>(framesOver),
                    static_cast<unsigned long long>(framesClear), over() ? "yes" : "no", static_cast<unsigned long long>(declinesTotal), static_cast<unsigned long long>(ringFull),
                    static_cast<unsigned long long>(perFrameFull), static_cast<unsigned long long>(probeFaults), static_cast<unsigned long long>(unavailable),
                    static_cast<unsigned long long>(maskLate), static_cast<unsigned long long>(depthMade), static_cast<unsigned long long>(depthTested), static_cast<unsigned long long>(depthIssued),
                    static_cast<unsigned long long>(depthFaults), static_cast<unsigned long long>(depthUnavailable), static_cast<unsigned long long>(writeBackDeclined),
                    s.open && !depthTested && !depthIssued ? " (NO sprite was tested or written in this window: the depth never ran)"
                    : depthTested && !depthIssued ? " (sprites were tested against a depth nothing wrote: the depth issues never ran)" : "");
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
    const bool galaxy = mapCameraGalaxyOpen();
    const bool system = mappointer::systemMapOn(galaxy, s.armed.load(std::memory_order_acquire), s.systemReturn, mapCameraAnyOpen());
    mapui::setSystemOpen(system);
    if (system) {
        MapCameraPose pose;
        if (mapCameraPose(pose)) {
            // The arrow's ray in the map's axes, as pointerCall reads it. The camera's forward until the cursor is known.
            float ray[3] = {pose.rows[2][0], pose.rows[2][1], pose.rows[2][2]};
            const uintptr_t cursor = s.cursorObject.load(std::memory_order_acquire);
            uintptr_t scene = 0;
            float arrow[3], d[3];
            if (cursor && readMem(cursor + kCursorSceneOffset, &scene, sizeof(scene)) && scene && readMem(scene + kScenePositionOffset, arrow, sizeof(arrow)) && arrowDirection(arrow, d))
                for (int k = 0; k < 3; ++k) ray[k] = d[0] * pose.rows[0][k] + d[1] * pose.rows[1][k] + d[2] * pose.rows[2][k];
            mapui::updateArrowDepth(pose.position, ray, pose.rows[2]);
        }
    }
    if (galaxy) ++s.galaxyFrames;
    if (!s.armed.load(std::memory_order_acquire)) {
        mapui::setGates(galaxy, false);   // the icons, labels and rings are fixed in the world whether or not the pointer's hooks can be had
        if (s.stood) return;
        if (s.tries && now - s.lastTryMs < 1000) return;
        s.lastTryMs = now;
        ++s.tries;
        bool transient = false;
        if (!tryInstall(s, transient)) {
            if (!transient || s.tries >= kMaxTries) {
                s.stood = true;
                Log::get().note("map pointer: stands down after %u tries (%s): the hover, the arrow's depth and the panel mask stay as the game made them (the icons, labels and rings are still fixed in the world)%s", s.tries, s.why,
                                s.cursor.hook.installed() ? "; the cursor capture stays in as a harmless store of one register" : "");
            }
            return;
        }
        s.armed.store(true, std::memory_order_release);
        s.lastLineMs = now;
        Log::get().note("map pointer: armed (VR profile): the cursor object capture is in at +0x%llX and the projector relay at +0x%llX, gate closed until a map opens; "
                        "the pointer's calls (the only ones replaced) return to +0x%llX on the Galaxy Map and +0x%llX on the System Map%s",
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s.cursor.hook.target()) - s.base),
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s.project.hook.target()) - s.base), static_cast<unsigned long long>(s.pointerReturn - s.base),
                        static_cast<unsigned long long>(s.systemReturn ? s.systemReturn - s.base : 0),
                        s.systemReturn ? "" : " (not found in this build: the System Map stays as the game made it)");
    }
    // The hover and the arrow on either map: the arrow's direction is the same ray in both (docs/galaxy-map-vr-2026-11-03.md, "The System Map"). The fixed icons and the sprite depth stay the Galaxy Map's.
    const bool open = galaxy || system;
    if (open) ++s.openFrames;
    if (open != s.open) {
        if (open) s.openedInWindow = true;
        s.open = open;
        if (s.edgeNotes < 16) {
            ++s.edgeNotes;
            Log::get().note("map pointer: a map %s (%s): the hover reads the arrow's direction, the arrow is at star depth off the panels and the game's on them, and the panel mask is %s",
                            open ? "opened" : "closed", galaxy ? "the Galaxy Map" : open ? "the System Map" : "none now", open ? "kept" : "let go");
        }
    }
    s.gate.store(open ? 1 : 0, std::memory_order_release);
    mapui::setGates(galaxy, open);
    if (!s.cursorSaid && s.cursorObject.load(std::memory_order_acquire)) {
        s.cursorSaid = true;
        Log::get().note("map pointer: the cursor update ran: its object is at %p", reinterpret_cast<void*>(s.cursorObject.load(std::memory_order_acquire)));
    }
    if (now - s.lastLineMs >= kLineMs) thirtySecondLine(s, now);
}

} // namespace edvr
