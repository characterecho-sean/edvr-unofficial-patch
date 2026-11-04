// The SRV radar's anchor moved in the game (issue 140; key experimental.srv_turret_radar_anchor = on, reversed or camera; docs\srv-turret-hud-2026-10-30.md, "the radar placement moved
// through its setter" and "the radar's own camera moved back").
//
// The Scorpion's radar disc is a game hologram: the radar's terrain schematic is drawn from the live camera into a texture and shown on a billboard (vs 5559BD94). At the stock
// turret camera the schematic's volume lies about 0.44 m BEHIND the eye, so the game's visibility gate keeps the picture's camera frozen and the picture is empty. This moves the
// volume forward in the game itself, so the game frames, renders and billboards it right. Two hooks, build 334925 only (any other build, or a prologue that differs, stands a hook
// down and patches nothing):
//   * the placement setter, for on and reversed (0x38563A0, interface slot +0x28 of the schematic's interface at obj+0x58; RCX = obj+0x58, RDX = the 4x4 P whose translation row is the
//     hologram's place relative to the anchor, in the anchor's axes): the hook ADDS the offset to P's translation and calls the original with the sum (srv_turret_math.h
//     radarPlacementMove). The game's own translation (about 1.96 m up on the Scorpion) is kept: the first build replaced it and dropped the radar. The offset is s * F along the
//     turret's seated forward u, put in the anchor's axes with the anchor's world rotation (its vfunc +0x20; srv_turret_math.h radarAnchorAlong); without u or a usable rotation it
//     is the fallback (0, 0, s * F), along the vehicle's forward, which swings the disc off as the turret turns (flight of 07bde936).
//   * the schematic slot's camera, for camera mode (THE SLOT CAMERA below): the placement stays the game's own and the radar's schematic slots are framed from a camera moved back
//     instead.
//
// The frame thread (srv_turret.cpp srvTurretFrameTick) arms the one the mode needs, publishes the offset every frame and does all the logging; the hooks only record.
#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {

enum class RadarAnchorHookState : int { NotTried = 0, Armed, StoodDown };

// Once (later calls return the first answer): verify the build and the setter's prologue and install the hook. Writes the log line "srv turret radar anchor: setter hook armed: ..." or
// "... setter hook stood down: <why>; nothing was patched". why (when given) receives the setter's line or reason. Frame thread.
RadarAnchorHookState srvRadarAnchorArm(char* why, size_t cap);
RadarAnchorHookState srvRadarAnchorState();
// The reason it stood down, or the armed line ("" before an arm).
const char* srvRadarAnchorWhy();

// Every frame, frame thread: the fallback offset's bit patterns ((0, 0, s * F) in the anchor's axes; all zero = no move: the next setter call gets the game's own placement) and the frame
// tick's frame number (the setter hook stamps an add with it).
void srvRadarAnchorPublish(const uint32_t bits[3], uint32_t frameNo);
// The turret's seated forward in the game's world (srv_turret_math.h turretForwardWorld), unit, from a camera sample of host frame seq (cpu: rows read when the game wrote them; else
// read back from the GPU) and the QPC time of the camera write it came from; null or !valid clears it. Render thread, and the frame tick clears it when a stay begins. The setter hook
// pairs it with the anchor's rotation of the same host frame and adds s * F along it as it is, with no lead; without u or a usable rotation it adds the fallback (0, 0, s * F). The slot
// camera hook takes its direction and the sample's age from it. cpu and headPitchDeg are not recorded.
void srvRadarAnchorPublishDirection(const float u[3], bool valid, uint64_t seq = 0, bool cpu = false, float headPitchDeg = 0.0f, int64_t qpc = 0);

// The setter hook's record: the counters are cumulative; gameT, caller and obj are those of the last radar setter call, outT, d and lastAddStamp those of the last add.
struct RadarAnchorStats {
    RadarAnchorHookState state = RadarAnchorHookState::NotTried;
    uint64_t moved = 0;            // the offset added (srv_turret_math.h PlacementMove::kAdded)
    uint64_t ours = 0;             // our own value fed back, passed through (kOurs)
    uint64_t restored = 0;         // our value fed back with the offset gone: the game's translation put back (kRestored)
    uint64_t faulted = 0;          // reads of a candidate object or matrix that faulted
    uint64_t noSlot = 0;           // a fifth schematic at once: left as the game set it
    bool holdsOurs = false;        // the last radar setter call left our offset in the placement (added or ours)
    bool haveGame = false;         // gameT has been read
    float gameT[3] = {};           // the game's own translation at the last radar setter call (the base a fed-back value came from)
    float outT[3] = {};            // what the last add gave the setter
    uint32_t lastAddStamp = 0;     // the frame number + 1 of the last add (0: never)
    uintptr_t caller = 0;          // the last radar setter call's return address
    uintptr_t callerRva = 0;       // ... minus the module base (0: not inside the module)
    uintptr_t obj = 0;             // the schematic (obj, not obj+0x58) of the last radar setter call
    float d[3] = {};               // the offset the last add used, in the anchor's axes
};
RadarAnchorStats srvRadarAnchorStats();

// THE SLOT CAMERA (experimental.srv_turret_radar_anchor = camera; docs\srv-turret-hud-2026-10-30.md, "the radar's own camera moved back"). The second hook, build 334925 only, on the
// schematic slot's update 0x36BF3C0 (RCX = the slot, RDX = the camera it frames, gates, billboards and renders its picture from): for the radar's slots (srv_turret_math.h
// radarSlotTest: the terrain disc's and the Scorpion icon's, both lists, so both eyes) while a back distance is published, the original gets a private copy of the camera moved back by it
// along the turret's forward (st::radarCameraBack, st::cameraMoveBy); every other call gets the game's camera. Once (later calls return the first answer): verify the build and the
// prologue and install it; says "srv turret radar camera: slot hook armed: ..." or "... slot hook stood down: <why>; nothing was patched". Frame thread.
RadarAnchorHookState srvRadarSlotArm();
// Every frame, frame thread: the back distance in metres (0: every call gets the game's camera).
void srvRadarSlotPublish(float backM);
// The slot hook's record, cumulative. Radar slots and first hits by kind (0 terrain, mode 4; 1 icon, mode 5; 2 another mode) and list (0 A, 1 B, 2 another).
struct RadarSlotFirst {
    bool have = false;
    uintptr_t slot = 0;
    uint32_t index = 0, mode = 0, list = 0;
    bool fromTurret = false;   // the direction: the turret's (the CPU camera sample), else the camera's own forward
    float u[3] = {}, back = 0.0f;
};
struct RadarSlotStats {
    RadarAnchorHookState state = RadarAnchorHookState::NotTried;
    uint64_t calls = 0, radar[3][3] = {}, moved = 0, fromTurret = 0, fromCamera = 0, faults = 0;
    uint32_t thread = 0;   // the thread the hook runs on (0: not called yet)
    RadarSlotFirst first[3][3];
};
RadarSlotStats srvRadarSlotStats();

// DLL unload: every gate closes, so the relays forward straight to the game's functions.
void srvRadarAnchorShutdown();

}  // namespace edvr
