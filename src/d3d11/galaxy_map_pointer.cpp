#include "galaxy_map_pointer.h"

#include <windows.h>

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
#include "map_ui_depth.h"

namespace edvr {

std::atomic<bool> g_mapUiDepthOpen{false};

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
// The projector's fields the self-check reads: its half extents at +0x80, the camera object at +8 whose projection rows are at +0x420 and view rows at
// +0x460 (0x40 a row set, indexed by the call's idx).
constexpr uintptr_t kProjectExtentsOffset = 0x80;
constexpr uintptr_t kProjectCameraOffset = 8;
constexpr uintptr_t kCameraProjectionOffset = 0x420;
constexpr uintptr_t kCameraViewOffset = 0x460;
constexpr uintptr_t kCameraSetStride = 0x40;
constexpr int kCameraSets = 8;

constexpr uint64_t kCheckWindowMs = 10000;   // the self-check: the first ten seconds of each opening,
constexpr uint64_t kCheckPeriodMs = 1000;    // once a second
constexpr uint64_t kLineMs = 30000;
constexpr unsigned kMaxOtherCallers = 8;
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

    // The windows: set by the frame thread at an opening, read by the projector detour.
    std::atomic<uint64_t> openedMs{0};
    std::atomic<uint64_t> lastCheckMs{0};

    // Counters, since the last 30 s line.
    std::atomic<uint64_t> pointerCalls{0};
    std::atomic<uint64_t> replaced{0};
    std::atomic<uint64_t> passed[kPassCount] = {};
    std::atomic<uint64_t> otherCalls{0};
    std::atomic<uint32_t> otherSeen{0};
    uintptr_t otherReturn[kMaxOtherCallers] = {};

    // Frame thread only.
    unsigned tries = 0;
    uint64_t lastTryMs = 0;
    bool stood = false;
    bool open = false;
    unsigned edgeNotes = 0;
    bool openedInWindow = false;
    uint64_t lastLineMs = 0;
    bool cursorSaid = false;
    bool boundSaid = false;
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

// The self-check, one line per second in the window: what the game passed against what the arrow's direction gives, with no model assumed, and the
// matrices K projects with.
void logCheck(void* k, int idx, const float* game, const MapCameraPose& pose, const float* arrow, const float* gameView, uint64_t gameRet,
              const float* oursView, uint64_t oursRet) noexcept {
    float rel[3];
    for (int i = 0; i < 3; ++i) rel[i] = game[i] - pose.position[i];
    float axes[3];
    inMapAxes(pose.rows, rel, axes);
    const float relLength = std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
    const float arrowLength = std::sqrt(arrow[0] * arrow[0] + arrow[1] * arrow[1] + arrow[2] * arrow[2]);
    float extents[4] = {};
    const bool extentsRead = readMem(reinterpret_cast<uintptr_t>(k) + kProjectExtentsOffset, extents, sizeof(extents));
    Log::get().note("map pointer check: the game's point is %.3f %.3f %.3f from the camera (length %.3f), in the map's axes a %.4f b %.4f c %.4f; the arrow P is %.4f %.4f %.4f "
                    "(length %.4f); a/Px %.3f b/Py %.3f c/Pz %.3f; the game's input projects to %.2f %.2f %.2f %.2f (returned %llu), ours to %.2f %.2f %.2f %.2f "
                    "(returned %llu); projector extents %s%.3f %.3f %.3f %.3f; idx %d",
                    rel[0], rel[1], rel[2], relLength, axes[0], axes[1], axes[2], arrow[0], arrow[1], arrow[2], arrowLength, ratio(axes[0], arrow[0]), ratio(axes[1], arrow[1]),
                    ratio(axes[2], arrow[2]), gameView[0], gameView[1], gameView[2], gameView[3], static_cast<unsigned long long>(gameRet & 0xFF), oursView[0], oursView[1],
                    oursView[2], oursView[3], static_cast<unsigned long long>(oursRet & 0xFF), extentsRead ? "" : "(unreadable) ", extents[0], extents[1], extents[2], extents[3], idx);
    float projection[16] = {}, view[16] = {};
    uintptr_t camera = 0;
    bool read = idx >= 0 && idx < kCameraSets && readMem(reinterpret_cast<uintptr_t>(k) + kProjectCameraOffset, &camera, sizeof(camera)) && camera;
    read = read && readMem(camera + kCameraProjectionOffset + static_cast<uintptr_t>(idx) * kCameraSetStride, projection, sizeof(projection)) &&
           readMem(camera + kCameraViewOffset + static_cast<uintptr_t>(idx) * kCameraSetStride, view, sizeof(view));
    if (!read) {
        Log::get().note("map pointer check: the projector's matrices for idx %d were not readable (camera object %p)", idx, reinterpret_cast<void*>(camera));
        return;
    }
    Log::get().note("map pointer check: idx %d projection rows (%.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g) "
                    "view rows (%.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g)",
                    idx, projection[0], projection[1], projection[2], projection[3], projection[4], projection[5], projection[6], projection[7], projection[8], projection[9],
                    projection[10], projection[11], projection[12], projection[13], projection[14], projection[15], view[0], view[1], view[2], view[3], view[4], view[5], view[6],
                    view[7], view[8], view[9], view[10], view[11], view[12], view[13], view[14], view[15]);
}

bool selfCheckDue(State& s) noexcept {
    const uint64_t now = GetTickCount64();
    if (now - s.openedMs.load(std::memory_order_acquire) >= kCheckWindowMs) return false;
    uint64_t last = s.lastCheckMs.load(std::memory_order_acquire);
    if (last && now - last < kCheckPeriodMs) return false;
    return s.lastCheckMs.compare_exchange_strong(last, now, std::memory_order_acq_rel);
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
    const bool check = selfCheckDue(s);
    float gameView[4] = {};
    uint64_t gameRet = 0;
    if (check) gameRet = original(k, in, idx, gameView);
    const uint64_t ret = original(k, farIn, idx, out);
    s.replaced.fetch_add(1, std::memory_order_relaxed);
    if (check) {
        float oursView[4] = {};
        readMem(reinterpret_cast<uintptr_t>(out), oursView, sizeof(oursView));
        logCheck(k, idx, game, pose, arrow, gameView, gameRet, oursView, ret);
    }
    return ret;
}

// A call from anywhere else with a point near the camera is a second test of the pointer (a click): its return address is said once.
void noteOtherCaller(State& s, uintptr_t caller, const float* in) noexcept {
    s.otherCalls.fetch_add(1, std::memory_order_relaxed);
    float p[3];
    if (!readMem(reinterpret_cast<uintptr_t>(in), p, sizeof(p))) return;
    MapCameraPose pose;
    if (!mapCameraPose(pose) || !nearCamera(p, pose.position, kNearCamera)) return;
    const uint32_t seen = s.otherSeen.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < seen && i < kMaxOtherCallers; ++i)
        if (s.otherReturn[i] == caller) return;
    const uint32_t slot = s.otherSeen.fetch_add(1, std::memory_order_acq_rel);
    if (slot >= kMaxOtherCallers) return;
    s.otherReturn[slot] = caller;
    Log::get().note("map pointer: another caller of the projector passed a point %.2f %.2f %.2f from the camera: return address +0x%llX (a second test of the pointer?)",
                    p[0] - pose.position[0], p[1] - pose.position[1], p[2] - pose.position[2], static_cast<unsigned long long>(caller - s.base));
}

// Reached through the relay's jump, so _ReturnAddress() is the game's caller.
__declspec(noinline) uint64_t projectDetour(void* k, const float* in, int idx, float* out) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    State& s = *g_s;
    const auto original = reinterpret_cast<Project>(s.forward.load(std::memory_order_acquire));
    if (caller != s.pointerReturn) {
        noteOtherCaller(s, caller, in);
        return original(k, in, idx, out);
    }
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
    g_mapUiDepthOpen.store(false, std::memory_order_release);
    s.open = false;
}

void thirtySecondLine(State& s, uint64_t now) {
    using namespace mapuidepth;
    uint64_t bound[4], declined[kDeclineCount], boundTotal = 0, declinedTotal = 0;
    for (int i = 0; i < 4; ++i) { bound[i] = g_mapUiDepth.counters.bound[i].exchange(0, std::memory_order_relaxed); boundTotal += bound[i]; }
    for (uint32_t i = 0; i < kDeclineCount; ++i) { declined[i] = g_mapUiDepth.counters.declined[i].exchange(0, std::memory_order_relaxed); declinedTotal += declined[i]; }
    const uint64_t calls = s.pointerCalls.exchange(0, std::memory_order_relaxed);
    const uint64_t replaced = s.replaced.exchange(0, std::memory_order_relaxed);
    uint64_t passed[kPassCount], passedTotal = 0;
    for (uint32_t i = 0; i < kPassCount; ++i) { passed[i] = s.passed[i].exchange(0, std::memory_order_relaxed); passedTotal += passed[i]; }
    const uint64_t others = s.otherCalls.exchange(0, std::memory_order_relaxed);
    const bool wasOpen = s.openedInWindow;
    s.openedInWindow = s.open;
    s.lastLineMs = now;
    if (!wasOpen && !calls && !others && !boundTotal && !declinedTotal) return;
    char reasons[256] = "";
    size_t n = 0;
    for (uint32_t i = 0; i < kPassCount && n + 40 < sizeof(reasons); ++i)
        if (passed[i]) n += static_cast<size_t>(std::snprintf(reasons + n, sizeof(reasons) - n, "%s%s %llu", n ? ", " : "", kPassNames[i], static_cast<unsigned long long>(passed[i])));
    char declines[256] = "";
    n = 0;
    for (uint32_t i = 0; i < kDeclineCount && n + 40 < sizeof(declines); ++i)
        if (declined[i]) n += static_cast<size_t>(std::snprintf(declines + n, sizeof(declines) - n, "%s%s %llu", n ? ", " : "", declineName(i), static_cast<unsigned long long>(declined[i])));
    Log::get().note("map pointer: Galaxy Map %s over the last 30 s; hover: the pointer's projector call ran %llu times, %llu replaced by the arrow's far point, %llu passed through%s%s%s; "
                    "%llu other projector calls; cursor object %s; panels and arrow at star depth: %llu draws bound (C4B4B3 %llu, B10B03 %llu, 5DAD41 %llu, arrow 4EF6DD %llu), %llu declined%s%s%s",
                    s.open ? "open now" : wasOpen ? "was open" : "not open", static_cast<unsigned long long>(calls), static_cast<unsigned long long>(replaced),
                    static_cast<unsigned long long>(passedTotal), passedTotal ? " (" : "", reasons, passedTotal ? ")" : "", static_cast<unsigned long long>(others),
                    s.cursorObject.load(std::memory_order_acquire) ? "seen" : "never seen", static_cast<unsigned long long>(boundTotal), static_cast<unsigned long long>(bound[0]),
                    static_cast<unsigned long long>(bound[1]), static_cast<unsigned long long>(bound[2]), static_cast<unsigned long long>(bound[3]),
                    static_cast<unsigned long long>(declinedTotal), declinedTotal ? " (" : "", declines, declinedTotal ? ")" : "");
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
    if (!s.armed.load(std::memory_order_acquire)) {
        if (s.stood) return;
        if (s.tries && now - s.lastTryMs < 1000) return;
        s.lastTryMs = now;
        ++s.tries;
        bool transient = false;
        if (!tryInstall(s, transient)) {
            if (!transient || s.tries >= kMaxTries) {
                s.stood = true;
                Log::get().note("map pointer: stands down after %u tries (%s): the hover and the panels' depth stay as the game made them%s", s.tries, s.why,
                                s.cursor.hook.installed() ? "; the cursor capture stays in as a harmless store of one register" : "");
            }
            return;
        }
        s.armed.store(true, std::memory_order_release);
        s.lastLineMs = now;
        Log::get().note("map pointer: armed (VR profile): the cursor object capture is in at +0x%llX and the projector relay at +0x%llX, gate closed until the Galaxy Map opens; "
                        "the pointer's call returns to +0x%llX",
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s.cursor.hook.target()) - s.base),
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s.project.hook.target()) - s.base), static_cast<unsigned long long>(s.pointerReturn - s.base));
    }
    const bool open = mapCameraGalaxyOpen();
    if (open != s.open) {
        if (open) {
            s.lastCheckMs.store(0, std::memory_order_release);
            s.openedMs.store(now, std::memory_order_release);
            s.openedInWindow = true;
        }
        s.open = open;
        if (s.edgeNotes < 16) {
            ++s.edgeNotes;
            Log::get().note("map pointer: the Galaxy Map %s: hover from the arrow's direction and panels and arrow at star depth are %s", open ? "opened" : "closed", open ? "on" : "off");
        }
    }
    s.gate.store(open ? 1 : 0, std::memory_order_release);
    g_mapUiDepthOpen.store(open, std::memory_order_release);
    if (!s.cursorSaid && s.cursorObject.load(std::memory_order_acquire)) {
        s.cursorSaid = true;
        Log::get().note("map pointer: the cursor update ran: its object is at %p", reinterpret_cast<void*>(s.cursorObject.load(std::memory_order_acquire)));
    }
    if (!s.boundSaid) {
        using namespace mapuidepth;
        for (int i = 0; i < 4; ++i)
            if (g_mapUiDepth.counters.bound[i].load(std::memory_order_relaxed)) {
                s.boundSaid = true;
                Log::get().note("map pointer: the first panel or arrow draw was bound with the eye origin zeroed (row 275 of the scene block at VS slot 1), vertex shader %s",
                                i == 0 ? "C4B4B334B26E81A9" : i == 1 ? "B10B032BDFD46700" : i == 2 ? "5DAD41AA66757CE3" : "4EF6DDB075A927FA");
                break;
            }
    }
    if (now - s.lastLineMs >= kLineMs) thirtySecondLine(s, now);
}

} // namespace edvr
