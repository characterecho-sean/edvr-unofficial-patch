// The player's XInput gamepad, watched for the FSS bindings.
//
// Elite lets any action land on a "GamePad" device (the XInput pad), and
// the field's own FSS keys are exactly there: enter on GamePad_Back,
// quit on GamePad_FaceRight. The keyboard-only binds adoption skipped
// those with a log line; this module watches them. XInput only -- a
// HOTAS or other DirectInput device stays out of reach, and the theater's
// GuiFocus authority covers those paths at poll latency instead.
//
// xinput9_1_0.dll is loaded dynamically (it ships with Windows; the
// proxy adds no import), connected slots are rescanned every few seconds
// (XInputGetState on an EMPTY slot is documented-slow, so empties are
// probed rarely), and a press is an edge: held-this-tick and not the
// tick before, on any connected pad.
#pragma once

#include <cstdint>

namespace edvr {

// One pad binding: a button mask (all must be held) and/or a trigger
// (1 left, 2 right, held past the XInput threshold). Invalid means "not
// watched" and presses never fire.
struct XinputBinding {
    uint16_t buttons = 0;
    uint8_t  trigger = 0;
    bool     valid = false;
};

// Translate an Elite GamePad key name ("GamePad_Back", "GamePad_LTrigger")
// into a binding. False for names this build cannot map -- said once in
// the log with the raw name, so it can be reported and added.
bool xinputTranslate(const char* eliteKey, XinputBinding* out);

// Poll the connected pads. Call once per frame from the frame boundary;
// cheap for connected pads, throttled for empty slots.
void xinputWatchTick();

// Did this binding go from released to held since the previous tick, on
// any connected pad?
bool xinputPressed(const XinputBinding& b);

}  // namespace edvr
