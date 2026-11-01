// The SRV radar's anchor moved in the game (issue 140; TEMPORARY key fix.srv_turret_radar_anchor; docs\srv-turret-hud-2026-10-30.md, "the radar placement moved through its
// setter").
//
// The Scorpion's radar disc is a game hologram: the radar's terrain schematic is drawn from the live camera into a texture and shown on a billboard (vs 5559BD94). At the stock
// turret camera the schematic's volume lies about 0.44 m BEHIND the eye, so the game's visibility gate keeps the picture's camera frozen and the picture is empty. This moves the
// volume forward in the game itself, so the game frames, renders and billboards it right. Two hooks, build 334925 only (any other build, or a prologue that differs, stands them
// down and patches nothing):
//   * the placement setter (0x38563A0, interface slot +0x28 of the schematic's interface at obj+0x58; RCX = obj+0x58, RDX = the 4x4 P whose translation row is the hologram's
//     place relative to the anchor, in the anchor's axes): the hook ADDS the offset to P's translation and calls the original with the sum (srv_turret_math.h
//     radarPlacementMove). The game's own translation (about 1.96 m up on the Scorpion) is kept: the first build replaced it and dropped the radar. The offset is s * F along the
//     turret's seated forward u, put in the anchor's axes with the anchor's world rotation (its vfunc +0x20; srv_turret_math.h radarAnchorAlong); without u or a usable rotation it
//     is the fallback (0, 0, s * F), along the vehicle's forward, which swings the disc off as the turret turns (flight of 07bde936).
//   * the schematic's per-frame update (0x384A590, obj in RCX, dt in XMM1): counted only, to say the object is alive. It writes nothing.
//
// The frame thread (srv_turret.cpp srvTurretFrameTick) arms them, publishes the offset every frame and does all the logging; the hooks only record.
#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {

enum class RadarAnchorHookState : int { NotTried = 0, Armed, StoodDown };

// Once (later calls return the first answer): verify the build and both prologues and install the hooks. Writes the log lines "srv turret radar anchor: setter hook armed: ..." or
// "... setter hook stood down: <why>; nothing was patched", then the same for the update counter. Armed means the setter hook is in (the counter may have stood down alone).
// why (when given) receives the setter's line or reason. Frame thread.
RadarAnchorHookState srvRadarAnchorArm(char* why, size_t cap);
RadarAnchorHookState srvRadarAnchorState();
// The reason it stood down, or the armed line ("" before an arm).
const char* srvRadarAnchorWhy();

// Every frame, frame thread: the fallback offset's bit patterns ((0, 0, s * F) in the anchor's axes; all zero = no move: the next setter call gets the game's own placement) and the frame
// tick's frame number (the setter hook stamps an add with it).
void srvRadarAnchorPublish(const uint32_t bits[3], uint32_t frameNo);
// The turret's seated forward in the game's world (srv_turret_math.h turretForwardWorld), unit, from a camera sample of host frame seq (cpu: rows read when the game wrote them; else
// read back from the GPU), with the head's pitch then; null or !valid clears it. Render thread, and the frame tick clears it when a stay begins. The setter hook pairs it with the
// anchor's rotation of the same host frame, carries the turret's aim forward over the sample's age (srv_turret_math.h extrapolateAim) and adds s * F along the result; without u or
// a usable rotation it adds the fallback (0, 0, s * F).
void srvRadarAnchorPublishDirection(const float u[3], bool valid, uint64_t seq = 0, bool cpu = false, float headPitchDeg = 0.0f);
// The 10 s line: the window's largest u age (frames) and extrapolation (degrees), reset by the read.
void srvRadarAnchorTakeWindowMax(uint64_t* ageMax, double* extrapMaxDeg);

struct RadarAnchorStats {
    RadarAnchorHookState state = RadarAnchorHookState::NotTried;
    bool updateArmed = false;      // the update counter is in
    uint64_t setterCalls = 0;      // setter calls through the gate
    uint64_t setterRadar = 0;      // of those, with the radar schematic (both vtables match)
    uint64_t moved = 0;            // the offset added (srv_turret_math.h PlacementMove::kAdded)
    uint64_t ours = 0;             // our own value fed back, passed through (kOurs)
    uint64_t restored = 0;         // our value fed back with the offset gone: the game's translation put back (kRestored)
    uint64_t faulted = 0;          // reads of a candidate object or matrix that faulted
    uint64_t noSlot = 0;           // a fifth schematic at once: left as the game set it
    uint64_t gameChanges = 0;      // the game's own translation differed from its previous one
    uint64_t updates = 0;          // the update's calls with the radar schematic
    bool holdsOurs = false;        // the last radar setter call left our offset in the placement (added or ours)
    bool haveGame = false;         // gameT has been read
    float gameT[3] = {};           // the game's own translation at the last radar setter call (the base a fed-back value came from)
    float outT[3] = {};            // what the last add gave the setter
    uint32_t lastAddStamp = 0;     // the frame number + 1 of the last add (0: never)
    uintptr_t caller = 0;          // the last radar setter call's return address
    uintptr_t callerRva = 0;       // ... minus the module base (0: not inside the module)
    uintptr_t obj = 0;             // the schematic (obj, not obj+0x58) of the last radar setter call
    bool suspect = false;          // the game's translation once moved by about the offset between two calls
    float suspectStep[3] = {};     // that step
    // the direction (srv_turret_math.h radarAnchorAlong): adds along the turret's forward (u and the anchor's rotation) against the fallback (0, 0, s * F), and why
    uint64_t directed = 0, fallback = 0, fallbackNoU = 0, fallbackNoAnchor = 0, anchorFaults = 0;
    bool lastDirected = false;     // the last add's source
    bool haveAnchor = false;       // anchorRows was read at the last radar call
    float anchorRows[9] = {};      // the anchor's world rotation rows (its vfunc +0x20) at the last radar call
    float pRows[9] = {};           // P's rotation rows at the last radar call
    float u[3] = {};               // the turret's forward in the world at the last add (zero: none)
    float uAnchor[3] = {};         // u in the anchor's axes at the last directed add (after the extrapolation)
    float d[3] = {};               // the offset the last add used, in the anchor's axes
    // the pairing and the extrapolation at the last directed computation
    uint64_t nowSeq = 0, sampleSeq = 0, age = 0;   // the setter's host frame, the camera sample's, and the difference
    float dyaw = 0.0f, dpitch = 0.0f;              // degrees carried forward
    float rateDps = 0.0f;                          // the turret's yaw rate relative to the vehicle, deg/s
    float headPitch = 0.0f, aimPitch = 0.0f, aimYaw = 0.0f;   // the head's pitch in the seat at the sample; the turret's aim relative to the vehicle
    bool sampleCpu = false, paired = false, lastClamped = false;   // the sample's source; the anchor's rotation found for the sample's frame; the step held
    uint64_t ageSum = 0, extrapMilliSum = 0, clamped = 0, pairedCount = 0, unpairedCount = 0;   // cumulative, over directed computations
};
RadarAnchorStats srvRadarAnchorStats();

// DLL unload: both gates close, so the relays forward straight to the game's functions.
void srvRadarAnchorShutdown();

}  // namespace edvr
