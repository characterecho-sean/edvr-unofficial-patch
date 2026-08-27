#include "xinput_watch.h"

#include <cstring>

#include <windows.h>

#include <Xinput.h>

#include "../common/log.h"
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

struct Slot {
    bool         connected = false;
    bool         seenNoted = false;
    XINPUT_STATE prev = {};
    XINPUT_STATE cur = {};
    uint64_t     probeMs = 0;
};
Slot g_slot[4];

struct PadMap {
    const char* elite;
    uint16_t    buttons;
    uint8_t     trigger;
};
// Elite's GamePad key names, from a field bindings file (FaceRight is B:
// the faces are named by position). Variants seen in the wild included.
constexpr PadMap kPadMap[] = {
    {"GamePad_FaceDown", XINPUT_GAMEPAD_A, 0},
    {"GamePad_FaceRight", XINPUT_GAMEPAD_B, 0},
    {"GamePad_FaceLeft", XINPUT_GAMEPAD_X, 0},
    {"GamePad_FaceUp", XINPUT_GAMEPAD_Y, 0},
    {"GamePad_A", XINPUT_GAMEPAD_A, 0},
    {"GamePad_B", XINPUT_GAMEPAD_B, 0},
    {"GamePad_X", XINPUT_GAMEPAD_X, 0},
    {"GamePad_Y", XINPUT_GAMEPAD_Y, 0},
    {"GamePad_DPadUp", XINPUT_GAMEPAD_DPAD_UP, 0},
    {"GamePad_DPadDown", XINPUT_GAMEPAD_DPAD_DOWN, 0},
    {"GamePad_DPadLeft", XINPUT_GAMEPAD_DPAD_LEFT, 0},
    {"GamePad_DPadRight", XINPUT_GAMEPAD_DPAD_RIGHT, 0},
    {"GamePad_Back", XINPUT_GAMEPAD_BACK, 0},
    {"GamePad_Start", XINPUT_GAMEPAD_START, 0},
    {"GamePad_LBumper", XINPUT_GAMEPAD_LEFT_SHOULDER, 0},
    {"GamePad_RBumper", XINPUT_GAMEPAD_RIGHT_SHOULDER, 0},
    {"GamePad_LShoulder", XINPUT_GAMEPAD_LEFT_SHOULDER, 0},
    {"GamePad_RShoulder", XINPUT_GAMEPAD_RIGHT_SHOULDER, 0},
    {"GamePad_LThumb", XINPUT_GAMEPAD_LEFT_THUMB, 0},
    {"GamePad_RThumb", XINPUT_GAMEPAD_RIGHT_THUMB, 0},
    {"GamePad_LStick", XINPUT_GAMEPAD_LEFT_THUMB, 0},
    {"GamePad_RStick", XINPUT_GAMEPAD_RIGHT_THUMB, 0},
    {"GamePad_LTrigger", 0, 1},
    {"GamePad_RTrigger", 0, 2},
};

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
    // the trigger threshold reads the positive direction either way.
    if (_strnicmp(eliteKey, "Pos_", 4) == 0 ||
        _strnicmp(eliteKey, "Neg_", 4) == 0) {
        eliteKey += 4;
    }
    for (const PadMap& m : kPadMap) {
        if (_stricmp(eliteKey, m.elite) == 0) {
            out->buttons = m.buttons;
            out->trigger = m.trigger;
            out->valid = true;
            return true;
        }
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
    for (DWORD i = 0; i < 4; ++i) {
        Slot& s = g_slot[i];
        if (!s.connected && !dueMs(s.probeMs, kProbeMs)) continue;
        if (!s.connected) s.probeMs = stampMs();
        s.prev = s.cur;
        XINPUT_STATE st = {};
        if (g_getState(i, &st) == ERROR_SUCCESS) {
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
}

bool xinputPressed(const XinputBinding& b) {
    if (!b.valid || !g_getState) return false;
    for (const Slot& s : g_slot) {
        if (!s.connected) continue;
        if (held(s.cur, b) && !held(s.prev, b)) return true;
    }
    return false;
}

}  // namespace edvr
