#pragma once
// The player's press of Elite's turret-view key, watched and never consumed (issue 179; docs/srv-turret-hud-2026-10-30.md, "2026-11-03: the press").
//
// Elite's binding element is ToggleBuggyTurretButton. It is read from the player's active .binds through the parser the HMD recentre key uses (elite_binds.h eliteBindsAction), each bound
// slot is watched with the machinery hotkeys already use (hotkey.h: the keyboard with its modifiers, a mouse button 1 to 5, the XInput pad, a joystick button the game is seen reading
// through DirectInput), and nothing here opens a device or touches the game's input. The bindings are re-read when the files change (eliteBindsFingerprint, two equal looks).
#include <cstdint>

namespace edvr {

struct SrvTurretPressState {
    bool pressed = false;        // a press edge of a watched slot was seen by this call
    bool observable = false;     // the action is bound and every bound slot can be watched (a pad or joystick slot not yet seen to press still counts: the blind rule catches it)
    uint32_t epoch = 0;          // increments each time the bindings are (re)read: a change of answer for the consumer
    const char* why = "";        // when not observable: the short reason, for the consumer's one log line
};

// Once a frame, on the frame thread (srvTurretFrameTick), VR only. Cheap when nothing changes.
SrvTurretPressState srvTurretPressPoll();

namespace srv_turret {
namespace pressmsg {
// The reasons a press cannot be seen, shared with the rig's wiring pins.
constexpr const char* kUnreadable = "no readable .binds";
constexpr const char* kReadingOff = "hotkey.read_game_bindings is off";
constexpr const char* kUnbound = "the action is not bound";
constexpr const char* kUnwatchable = "a bound slot is a device or key nothing here watches";
}  // namespace pressmsg
}  // namespace srv_turret

}  // namespace edvr
