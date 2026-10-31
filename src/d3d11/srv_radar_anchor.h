// The SRV radar's anchor moved in the game (issue 140; TEMPORARY key fix.srv_turret_radar_anchor; docs\srv-turret-hud-2026-10-30.md, "the radar anchor moved in the game").
//
// The Scorpion's radar disc is a game hologram: the radar's terrain schematic is drawn from the live camera into a texture and shown on a billboard (vs 5559BD94). At the stock
// turret camera the schematic's volume lies about 0.44 m BEHIND the eye, so the game's visibility gate keeps the picture's camera frozen and the picture is empty. This moves the
// volume forward in the game itself, so the game frames, renders and billboards it right: an entry hook on the schematic's per-frame update (0x384A590 in build 334925, obj in RCX,
// dt in XMM1) writes a local offset into the two translation rows its world-matrix writer reads (obj+0x15B0 and obj+0x15F0, metres in the anchor entity's own axes; the
// constructor zeroes them and nothing else in the game writes them). Build 334925 only: any other build, or a prologue that differs, stands the hook down and patches nothing.
//
// The frame thread (srv_turret.cpp srvTurretFrameTick) arms it, publishes the offset every frame and does all the logging; the hook itself only stores integers.
#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {

enum class RadarAnchorHookState : int { NotTried = 0, Armed, StoodDown };

// Once (later calls return the first answer): verify the build and the prologue and install the hook. Writes one log line: "srv turret radar anchor: hook armed: ..." or
// "srv turret radar anchor: hook stood down: <why>; nothing was patched". why (when given) receives the reason or the armed line. Frame thread.
RadarAnchorHookState srvRadarAnchorArm(char* why, size_t cap);
RadarAnchorHookState srvRadarAnchorState();
// The reason it stood down, or the armed line ("" before an arm).
const char* srvRadarAnchorWhy();

// Every frame, frame thread: the offset's bit patterns (x, y, z in the anchor's axes; all zero = no move, and a schematic that holds one of ours gets zeros once) and the frame
// tick's frame number (the hook stamps an application with it). Opens the hook's gate while the offset is not zero or a schematic still holds one of ours, closes it otherwise.
void srvRadarAnchorPublish(const uint32_t bits[3], uint32_t frameNo);

struct RadarAnchorStats {
    RadarAnchorHookState state = RadarAnchorHookState::NotTried;
    uint64_t calls = 0;          // calls the gate let through to the hook
    uint64_t matched = 0;        // of those, with the radar terrain schematic (both vtables match)
    uint64_t applied = 0;        // offsets written (non-zero)
    uint64_t appliedFrames = 0;  // distinct frames with an offset written
    uint64_t restored = 0;       // zeros written back into a schematic that held one of ours
    uint64_t faulted = 0;        // reads of a candidate object that faulted
    uint64_t noSlot = 0;         // a fifth schematic at once: left as the game made it
    uint32_t lastStamp = 0;      // the frame number + 1 of the last application (0: never)
    uintptr_t lastObj = 0;       // the schematic last written
    bool holding = false;        // some schematic still holds an offset of ours
};
RadarAnchorStats srvRadarAnchorStats();

// DLL unload: the gate closes, so the relay forwards straight to the game's function.
void srvRadarAnchorShutdown();

}  // namespace edvr
