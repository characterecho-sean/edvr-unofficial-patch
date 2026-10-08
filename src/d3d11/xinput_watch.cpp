#include "xinput_watch.h"

#include <cstring>

#include <windows.h>

#include <Xinput.h>

#include "../common/log.h"
#include "../common/pad_names.h"
#include "../common/periodic_work.h"
#include "../common/timing.h"

namespace edvr {
namespace {

typedef DWORD(WINAPI* PFN_XInputGetState)(DWORD, XINPUT_STATE*);

PFN_XInputGetState g_getState = nullptr;
bool g_loadTried = false;
bool g_loadFailedNoted = false;

// Probe empty slots rarely: XInputGetState on a disconnected index is
// documented as expensive, and four of them per frame would be a tax on
// everyone who owns no pad.
constexpr uint64_t kProbeMs = 3000;

// Phase-0 timing (src/common/periodic_work.h): what one probing tick spends
// asking empty slots whether a pad has appeared. One sample per tick, not per
// slot -- every empty slot's clock starts on the same first frame, so they all
// come due together, and what a frame pays is their SUM. The context is how
// many slots that tick probed.
PeriodicWork g_workProbe{"xinput_probe", "slots"};

struct Slot {
    bool         connected = false;
    bool         seenNoted = false;
    XINPUT_STATE prev = {};
    XINPUT_STATE cur = {};
    uint64_t     probeMs = 0;
};
Slot g_slot[4];

// Elite's GamePad key names live in src/common/pad_names.h, shared with the
// hotkey parser; the masks there are XInput's own, checked here.
static_assert(kPadUp == XINPUT_GAMEPAD_DPAD_UP && kPadDown == XINPUT_GAMEPAD_DPAD_DOWN &&
                  kPadLeft == XINPUT_GAMEPAD_DPAD_LEFT && kPadRight == XINPUT_GAMEPAD_DPAD_RIGHT &&
                  kPadStart == XINPUT_GAMEPAD_START && kPadBack == XINPUT_GAMEPAD_BACK &&
                  kPadLeftThumb == XINPUT_GAMEPAD_LEFT_THUMB &&
                  kPadRightThumb == XINPUT_GAMEPAD_RIGHT_THUMB &&
                  kPadLeftShoulder == XINPUT_GAMEPAD_LEFT_SHOULDER &&
                  kPadRightShoulder == XINPUT_GAMEPAD_RIGHT_SHOULDER && kPadA == XINPUT_GAMEPAD_A &&
                  kPadB == XINPUT_GAMEPAD_B && kPadX == XINPUT_GAMEPAD_X && kPadY == XINPUT_GAMEPAD_Y,
              "pad_names.h's masks are XInput's");

bool ensureLoaded() {
    if (g_getState) return true;
    if (g_loadTried) return false;
    g_loadTried = true;
    for (const wchar_t* name :
         {L"xinput9_1_0.dll", L"xinput1_4.dll", L"xinput1_3.dll"}) {
        HMODULE m = LoadLibraryW(name);
        if (!m) continue;
        g_getState = reinterpret_cast<PFN_XInputGetState>(
            GetProcAddress(m, "XInputGetState"));
        if (g_getState) return true;
        FreeLibrary(m);
    }
    if (!g_loadFailedNoted) {
        g_loadFailedNoted = true;
        Log::get().note(
            "xinput: no XInput runtime could be loaded; gamepad FSS "
            "bindings will not be watched (the GuiFocus authority still "
            "covers them at poll latency).");
    }
    return false;
}

bool held(const XINPUT_STATE& st, const XinputBinding& b) {
    if (b.buttons &&
        (st.Gamepad.wButtons & b.buttons) != b.buttons) {
        return false;
    }
    if (b.trigger == 1 &&
        st.Gamepad.bLeftTrigger <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD) {
        return false;
    }
    if (b.trigger == 2 &&
        st.Gamepad.bRightTrigger <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD) {
        return false;
    }
    return b.buttons != 0 || b.trigger != 0;
}

}  // namespace

bool xinputTranslate(const char* eliteKey, XinputBinding* out) {
    if (!eliteKey || !out) return false;
    // Axis bindings carry a direction prefix ("Pos_GamePad_RTrigger");
    // padNameFind drops it, the trigger threshold reading the positive
    // direction either way.
    if (const PadName* m = padNameFind(eliteKey)) {
        out->buttons = m->buttons;
        out->trigger = m->trigger;
        out->valid = true;
        return true;
    }
    if (_strnicmp(eliteKey, "Pos_", 4) == 0 ||
        _strnicmp(eliteKey, "Neg_", 4) == 0) {
        eliteKey += 4;
    }
    static bool s_unmappedNoted = false;
    if (!s_unmappedNoted) {
        s_unmappedNoted = true;
        Log::get().note(
            "xinput: Elite names a gamepad key \"%s\" that this build has "
            "no XInput mapping for -- please report this line. The "
            "GuiFocus authority still covers the action.", eliteKey);
    }
    return false;
}

void xinputWatchTick() {
    if (!ensureLoaded()) return;
    // One poll per frame, however many callers ask. The FSS latch tick and the
    // hotkeys (a pad hotkey, the settings menu's capture) all call this, and
    // every real poll copies `cur` into `prev`: a second poll in the same frame
    // would make the frame's edge invisible to whoever reads second. The guard
    // is a few milliseconds, well under any frame time this runs at and well
    // over two calls from one frame.
    {
        const uint64_t now = stampMs();
        static uint64_t s_lastPollMs = 0;
        if (s_lastPollMs && now - s_lastPollMs < 3) return;
        s_lastPollMs = now;
    }
    int64_t probeTicks = 0;    // clock ticks spent in XInputGetState on empty slots
    uint32_t probedSlots = 0;  // and how many of them there were this tick
    for (DWORD i = 0; i < 4; ++i) {
        Slot& s = g_slot[i];
        if (!s.connected && !dueMs(s.probeMs, kProbeMs)) continue;
        // Decided before the call: a probe that finds a pad flips `connected`
        // below, and it was still a probe of an empty slot.
        const bool probingEmpty = !s.connected;
        if (probingEmpty) s.probeMs = stampMs();
        s.prev = s.cur;
        XINPUT_STATE st = {};
        const int64_t probeStart = probingEmpty ? qpcNow() : 0;
        const DWORD state = g_getState(i, &st);
        if (probingEmpty) {
            probeTicks += qpcNow() - probeStart;
            ++probedSlots;
        }
        if (state == ERROR_SUCCESS) {
            s.cur = st;
            if (!s.connected) {
                s.connected = true;
                s.prev = st;   // no phantom edge on the connect tick
                if (!s.seenNoted) {
                    s.seenNoted = true;
                    Log::get().note(
                        "xinput: a gamepad in slot %lu is being watched "
                        "for the FSS bindings.",
                        static_cast<unsigned long>(i));
                }
            }
        } else if (s.connected) {
            s.connected = false;
            s.probeMs = stampMs();
            s.cur = XINPUT_STATE{};
        }
    }
    // Only a tick that probed is a run. The ticks between probes (about 270 of
    // them at 90 Hz) poll connected pads, or nothing at all, and are not the
    // periodic work being priced here.
    if (probedSlots != 0) g_workProbe.recordDuration(probeTicks, probedSlots);
}

bool xinputPressed(const XinputBinding& b) {
    if (!b.valid || !g_getState) return false;
    for (const Slot& s : g_slot) {
        if (!s.connected) continue;
        if (held(s.cur, b) && !held(s.prev, b)) return true;
    }
    return false;
}

bool xinputHeld(const XinputBinding& b) {
    if (!b.valid || !g_getState) return false;
    for (const Slot& s : g_slot) {
        if (s.connected && held(s.cur, b)) return true;
    }
    return false;
}

void xinputSnapshot(uint16_t* buttons, uint8_t* triggers) {
    uint16_t b = 0;
    uint8_t t = 0;
    if (g_getState) {
        for (const Slot& s : g_slot) {
            if (!s.connected) continue;
            b = static_cast<uint16_t>(b | s.cur.Gamepad.wButtons);
            if (s.cur.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) t |= 1;
            if (s.cur.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) t |= 2;
        }
    }
    if (buttons) *buttons = b;
    if (triggers) *triggers = t;
}

}  // namespace edvr
