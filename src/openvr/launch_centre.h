// Making the tracking origin deterministic.
//
// WHAT IS WRONG
//
// OpenComposite's tracking origin lands somewhere different every launch.
// Measured 2026-08-29 on the field rig, two launches two minutes apart, the
// player not having moved, with fix.vr_handover = early so the session is
// already rebuilt before the game reads anything:
//
//     06:45  yaw=+0.1  pos -0.006 -0.000 -0.579
//     06:48  yaw=-0.0  pos +1.788 -0.001 -3.502
//
// The ORIENTATION is fixed and reliable -- yaw ~0 both times, against the
// yaw ~180 the same rig reported before the early handover. It is the
// POSITION that scatters, by metres, per session.
//
// Anything the game anchors in its world therefore lands somewhere new each
// launch: the splash, the loading screens, and the intro movie's panel,
// which is placed 3.35 m from the origin. On the two flights above that put
// the movie 2.77 m in front of the player, and then 0.15 m BEHIND them.
// SteamVR does not do this; the origin is stable there.
//
// WHAT THIS DOES
//
// Asks the runtime to move its own origin to where your head is, once, at
// the start of the session -- IVRSystem::ResetSeatedZeroPose, the same
// recentre the runtime already offers, called for you before the movie
// starts.
//
// It is the RUNTIME'S space that moves. Nothing is subtracted from the poses
// afterwards: OpenComposite builds a fresh XR_REFERENCE_SPACE_TYPE_LOCAL
// offset by the headset's current position and Y-rotation, swaps it in for
// its seated space, and every pose after that is naturally expressed in it.
// An ongoing per-frame correction from us would be a second, competing idea
// of where the world is; this leaves exactly one.
//
// This also gets the ORIENTATION for free, which a pose-editing approach
// could not: openvr.head_yaw_degrees is MEASURED DEAD in this project
// because the runtime's reprojection undoes a yaw written into a pose. Here
// the runtime is the thing being changed, so there is nothing left to undo
// it.
//
// WHAT IT DOES NOT DO
//
// It cannot help a game that asks for STANDING poses: OpenComposite's
// ResetSeatedZeroPose moves the seated space only. The log line after the
// call says what to compare so that case is recognisable rather than
// mysterious.
//
// It is also gated, inside OpenComposite, on IsGraphicsConfigured() -- and
// silently does nothing if the headset cannot be located when asked. Both
// are why this waits for a valid pose before spending its one call.
#pragma once

#include <cstdint>

#include "openvr_min.h"

namespace edvr {

class Config;

// Read fix.launch_centre. MUST be called at hook install as well as on
// reload -- head_offset.h documents at length the session this project lost
// to a configure() that only ran on reload, and tools/check_install_reads.py
// enforces it statically.
// Told the real runtime's module handle as soon as openvr_proxy.cpp has it,
// which is before the config exists -- so this only remembers the handle.
// The identification, and the line about it, happen in Configure.
//
// Passed in rather than looked up: the real module's FILENAME is a config
// value (advanced.real_openvr_dll, "openvr_api_oc.dll" on the rig this was
// written for), so there is nothing to GetModuleHandle for.
void launchCentreNoteRuntime(void* realModule);

void launchCentreConfigure();

// Ask the runtime to recentre, once, as soon as it can honour the request.
//
// `err` is WaitGetPoses' own return value: on anything but success the pose
// array may be untouched or stale, and the "is tracking up yet" test below
// would be reading nothing.
//
// Call immediately after the real WaitGetPoses returns, before the head pose
// is published and before headOffsetApply, so that the frame in which the
// origin moves is the same frame every reader sees it move.
//
// Waits for a headset pose the runtime vouches for before spending its one
// call: OpenComposite's ResetSeatedZeroPose does nothing at all if it cannot
// locate the view space, and a silent no-op is indistinguishable from a
// broken feature.
void launchCentreApply(vr::EVRCompositorError err,
                       vr::TrackedDevicePose_t* renderPoses,
                       uint32_t renderCount,
                       vr::TrackedDevicePose_t* gamePoses, uint32_t gameCount);

// Has the one call been spent this session -- whether it succeeded, faulted
// or timed out? For callers that want to say "already done" rather than
// asking again.
bool launchCentreLatched();

}  // namespace edvr
