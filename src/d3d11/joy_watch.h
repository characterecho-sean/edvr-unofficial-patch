// HOTAS and joystick buttons, WATCHED and never captured.
//
// Elite reads its joysticks through DirectInput, and input_gate.cpp already
// wraps GetDeviceState and GetDeviceData on the game's own device objects
// (for the keyboard). For every OTHER game-controller device those wrappers
// call into this module AFTER the game's own call has returned, with the
// buffer the game just got: the module copies the button bytes and the hat
// values out of it into a small table, and that is all it does. It never
// writes to the game's buffer, never calls a device method, and creates no
// DirectInput device of EDVR's own -- so the force-feedback stall of issue 45
// (a wheel's FFB driver inside DirectInput on the render thread) has no new
// traffic to come from.
//
// The hotkey machinery reads the table: hotkey.h's reader asks joyWatchHeld
// for a binding like "231D0200:Joy_12" and Hotkey makes the edge, with the
// same foreground rule a keyboard hotkey has.
//
// WHAT IT UNDERSTANDS, and ignores the rest (fail-open):
//   * GetDeviceState with the two standard joystick layouts, DIJOYSTATE (80
//     bytes) and DIJOYSTATE2 (272). Any other size is not guessed at.
//   * GetDeviceData rows at those layouts' offsets: DIJOFS_POV(n) = 32 + 4n
//     and DIJOFS_BUTTON(n) = 48 + n, which both layouts share. A custom data
//     format's rows land on offsets that mean something else and are dropped
//     by the same rule, counted, and named in the probe line.
//
// NAMES follow Elite's .binds: the device is "VVVVPPPP" (vendor, product) and
// the key is Joy_N (1-based) or Joy_POV1Up (hotkey.h numbers them once).
#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {

constexpr uint32_t kJoyStateSize = 80;     // sizeof(DIJOYSTATE)
constexpr uint32_t kJoyState2Size = 272;   // sizeof(DIJOYSTATE2)
constexpr uint32_t kJoyOfsPov = 32;        // DIJOFS_POV(0)
constexpr uint32_t kJoyOfsButtons = 48;    // DIJOFS_BUTTON(0)
constexpr int      kJoyWatchSlots = 8;

// DirectInput's product GUID has Data1 = product << 16 | vendor. Elite writes
// the id the other way round, "231D0200" for vendor 231D and product 0200,
// which hotkey.h keeps as vendor << 16 | product.
uint32_t joyDeviceIdFromProduct(uint32_t productData1);

// A device type the game enumerated as a game controller (the types a joystick,
// gamepad, wheel, flight stick, first-person controller, a generic device or a
// supplemental device -- pedals, a throttle -- reports; a mouse and a keyboard
// never). `type` is GET_DIDEVICE_TYPE(dwDevType) from the game's own EnumDevices record.
bool joyDeviceTypeIsController(uint32_t type);

// ---- the writer: the game's thread, from input_gate's wrappers ----------------

// GetDeviceState just returned `data` of `cb` bytes for `device`.
bool joyWatchObserveState(const void* device, uint32_t deviceId, const void* data, uint32_t cb,
                          uint64_t nowMs);

// GetDeviceData just returned `count` rows of `stride` bytes (each starts with
// DIDEVICEOBJECTDATA's dwOfs and dwData). A call that returns nothing still
// proves the game is reading the device, so it is passed with count 0.
bool joyWatchObserveData(const void* device, uint32_t deviceId, const void* rows, uint32_t count,
                         uint32_t stride, uint64_t nowMs);

// ---- the readers: the frame thread --------------------------------------------

// Is this input of this device held? False for a device the game has not been
// seen reading lately, and for a button that was already down when the game
// (re)started reading the device, until it has been seen released.
bool joyWatchHeld(uint32_t deviceId, uint16_t input, uint64_t nowMs);

// Every device's held inputs, for the capture: bits[0..1] are Joy_1..Joy_128,
// bits[2] the sixteen hat directions (hotkey.h's kJoyPovBase order).
struct JoySnapshot {
    struct Device {
        uint32_t id = 0;
        uint64_t bits[3] = {0, 0, 0};
    };
    int    count = 0;
    Device dev[kJoyWatchSlots];
};
void joyWatchSnapshot(JoySnapshot* out, uint64_t nowMs);
bool joyInputIn(const JoySnapshot::Device& d, uint16_t input);
// The lowest input that is held in `cur` and was not in `prev` -- a press
// since the last look, on any device. Pure.
bool joyFirstNew(const JoySnapshot& prev, const JoySnapshot& cur, uint32_t* deviceId, uint16_t* input);

// A hat value (hundredths of a degree, 0xFFFF when centred) read as one of
// the four directions: within a quarter turn of it, so a diagonal counts for
// both neighbours, as Elite reads a hat. Pure.
bool joyPovMatches(uint16_t value, int direction);

// ---- bookkeeping ---------------------------------------------------------------

struct JoyWatchStats {
    int      devices = 0;
    uint64_t stateCalls = 0;
    uint64_t dataCalls = 0;
    uint64_t rowsUsed = 0;
    uint64_t rowsIgnored = 0;     // rows at an offset that is neither a button nor a hat
    uint64_t statesIgnored = 0;   // a buffer of a size this does not read
    uint64_t tableFull = 0;
    uint32_t lastIgnoredSize = 0;
    uint32_t lastIgnoredOfs = 0;
};
void joyWatchStats(JoyWatchStats* out);
// Forget everything (a rig, between cases).
void joyWatchReset();

}  // namespace edvr
