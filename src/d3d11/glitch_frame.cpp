#include "glitch_frame.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/log.h"
#include "transition_flash_eye_base.h"
#include "transition_flash_eye_base_core.h"

// The camera-buffer tap of the transition-flash engine fix (glitch_frame.h).
//
// The old independent detector that lived here -- a jump chain over the camera's
// position, a separation memory, certified orbits and parks, a pool-coherence
// cross-check, a camera-history ring and its dump -- was removed on 2026-10-13
// (docs/transition-flash.md). It ran only when the engine fix was not armed, and
// it was dormant in every flight that mattered.
namespace edvr {
namespace {

// Where the viewpoint lives in the game's memory: the game's 5376-byte scene
// constant buffer, the camera origin at float 1100. Build-specific -- the engine
// fix arms only for the game build it was made for -- so these are the engine
// fix's own constants (they used to be two ini keys, removed 2026-10-14: nobody
// had a reason to move them, and a wrong pair only switched the hold off).
constexpr uint32_t kBufferBytes = tfeb::kSceneCBBytes;
constexpr uint32_t kPosOffset = static_cast<uint32_t>(tfeb::kSceneCBOriginFloat);
// Three floats have to fit at that offset, inside that buffer. Checked at compile
// time now that the pair cannot be edited.
static_assert(static_cast<uint64_t>(kPosOffset) * 4u + 12u <= kBufferBytes,
              "the camera origin (three floats at kPosOffset) must fit inside the scene constant buffer");

// The test rigs' switch to run the DLL with the tap off (a transport-only
// install: vScreen takes no Map/Unmap hooks and has no frame boundary). It was the
// advanced ini key "camera buffer size = 0", which a user could set and a stale
// line could carry; an environment variable is something no ini file holds. The
// value "1" switches it; anything else, or unset, leaves the tap on.
constexpr const char* kTestNoTapVariable = "EDVR_TEST_NO_CAMERA_TAP";

bool testSwitchesTapOff() {
    char value[4] = {};
    const DWORD n = GetEnvironmentVariableA(kTestNoTapVariable, value, sizeof value);
    return n == 1 && value[0] == '1';
}

struct State {
    // The camera buffer is observed: the tap is installed. Set on every install
    // that is not switched off by the test rigs' variable.
    bool observing = false;
    // Frames since install, advanced at the frame boundary. A fill carries the
    // PRE-advance value (the boundary that closes the frame records it one
    // higher); the engine fix compensates.
    uint32_t frameNo = 0;
};

State* g_state = nullptr;

// The engine fix reported Armed. Until it does (and for good when it cannot),
// the tap asks for no buffer and records nothing.
std::atomic<bool> g_engineFixArmed{false};
// The engine fix's event window is open (armed event .. its two bad renders):
// the camera CB's Unmap tap is the one place the fix looks at a frame.
std::atomic<bool> g_engineWindowOpen{false};

bool finite3(const float* p) {
    for (uint32_t a = 0; a < 3; ++a) {
        if (!std::isfinite(p[a])) return false;
    }
    return true;
}

}  // namespace

void glitchFrameEngineWindow(bool open) {
    g_engineWindowOpen.store(open, std::memory_order_relaxed);
}

void glitchFrameNoteEngineFix(bool armed) {
    g_engineFixArmed.store(armed, std::memory_order_relaxed);
}

// The engine fix marks frames from the camera-buffer tap inside
// glitchFrameObserve, so "armed" means the tap can deliver. It reaches the
// engine module only when the buffer is observed (size nonzero and the position
// offset inside it -- install below), Map nominates that buffer by size
// (kBufferBytes == the engine's 5376-byte scene CB), and the position passes the
// finite check per fill. This answers the static part.
bool glitchFrameEngineTapAvailable(char* why, size_t whySize) {
    State* s = g_state;
    if (!s) { snprintf(why, whySize, "the tap is not installed"); return false; }
    if (!s->observing) {
        snprintf(why, whySize, "the camera buffer is not observed -- the tap is switched off "
                 "(%s is set; the test rigs' switch)", kTestNoTapVariable);
        return false;
    }
    return true;
}

bool glitchFrameEngineFixHold() {
    // A camera jump: the FSS arrival trigger and the openvr half key on it.
    noteWorldJump();
    markGlitchFrame();
    // The jump verdict is NOT published here: see glitchFrameEngineFixVerdict.
    return glitchConsumerPresent();
}

void glitchFrameEngineFixVerdict() {
    // "Stayed": a change of reference frame, the next frame is coherent, so
    // the temporal pass keeps its history.
    noteJumpVerdict(2);
}

void installGlitchFrameFix() {
    static State s;
    g_state = &s;
    // A re-install on the same static State (the test rigs, a hot reload) must
    // not inherit a stale "observing" from a geometry that has since been refused.
    s.observing = false;

    if (testSwitchesTapOff()) {
        Log::get().note("transition flash: camera-buffer tap off: %s = 1 (the test rigs' switch).",
                        kTestNoTapVariable);
        return;
    }
    s.observing = true;
    // Said at install, so a log from a session that never reached a rendered
    // scene still shows whether the tap was set up at all.
    Log::get().note(
        "transition flash: camera-buffer tap ready: reading float %u of the %u-byte constant "
        "buffer while the engine fix has an event open.",
        kPosOffset, kBufferBytes);
}

bool glitchFrameWantsHooks() {
    State* s = g_state;
    return s && s->observing;
}

bool glitchFrameWantsBuffer(uint32_t bytes) {
    State* s = g_state;
    // One size compare per Map. Nominated whether or not the engine fix is
    // armed: vScreen's Unmap tee hands the same block to the sun-glare fix and
    // the temporal pass, which depend on this nomination. The tap itself stays
    // idle until an event window opens (glitchFrameObserve).
    return s && s->observing && bytes == kBufferBytes;
}

void glitchFrameObserve(const void* data, uint32_t bytes, const void* /*resource*/) {
    State* s = g_state;
    if (!s || !s->observing) return;
    // Idle: three relaxed flag tests, nothing recorded.
    if (!g_engineWindowOpen.load(std::memory_order_relaxed)) return;
    if (bytes != kBufferBytes) return;

    const float* pos = &static_cast<const float*>(data)[kPosOffset];
    // Non-finite values compare false in both directions, so a NaN here would
    // pass every threshold test silently rather than failing one.
    if (!finite3(pos)) return;

    // The engine fix's render tap (transition_flash_eye_base.cpp): it sees this
    // pre-Unmap fill while an event's window is open and holds the frame at the
    // first head-only one. The frame is s->frameNo -- the PRE-advance counter.
    transitionFlashEyeBaseNoteSceneCB(s->frameNo, data, bytes);
}

void glitchFrameBoundary() {
    State* s = g_state;
    if (!s || !s->observing) return;
    ++s->frameNo;
}

// NOT REACHED ON A NORMAL GAME EXIT, and nothing that matters may live here.
//
// shutdown() calls this, and DllMain calls shutdown() only for a FreeLibrary
// unload -- which does not happen to a game's d3d11.dll. Process exit takes the
// other branch on purpose: every other thread is already dead and may have been
// holding the log's spinlock, so that path must not take one. This runs for the
// test harness and for a hot unload, and that is all.
void shutdownGlitchFrameFix() {
    g_state = nullptr;
}

}  // namespace edvr
