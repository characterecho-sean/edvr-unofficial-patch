// The gaze probe: does this runtime answer OpenVR's eye-tracked foveation
// centre, and with what?
//
// WHY THIS EXISTS
//
// docs/performance.md, feature 3: every foveation design in this project --
// the shading-rate rings, and the DLSS-on-a-crop the temporal work made
// worth building -- wants one thing from the runtime, a per-eye point in
// the image where the player is looking. Valve added exactly that to
// OpenVR: IVRSystem::GetEyeTrackedFoveationCenter (SDK 2.15.6), fed by any
// headset driver that publishes gaze through the driver API (SDK 2.12.14).
// Whether the Pimax Crystal Super's driver does so under SteamVR is the
// whole question, and it cannot be answered from a header. This file asks
// the runtime, once a frame, and writes down what came back -- in
// aggregate, never a sample.
//
// HOW IT ASKS
//
// Through the C function table, not the C++ vtable. OpenVR serves every
// interface version in two shapes: "IVRSystem_026", a C++ object whose
// methods take a hidden `this` and return structs through a hidden
// pointer, and "FnTable:IVRSystem_026", a plain struct of C function
// pointers with no `this` at all. The 6c rule -- IVRSystem is never called
// from inside the game (system_hook.cpp) -- was earned by the first shape:
// a method returning a matrix by value, called with the wrong idea of the
// hidden return slot, corrupted the stack after appearing to work. The
// table has no hidden anything: the two calls made here return a bool and
// fill caller-owned floats, and the one made to validate the table returns
// void and fills two integers. The layout is transcribed from Valve's
// openvr_capi.h at the SDK whose IVRSystem_Version IS "IVRSystem_026", so
// the entry index and the version string come from one header rather
// than a guess across generations -- the failure the ban records.
//
// It is still a call into the runtime from inside the game, so it runs the
// way the launch centre's one call runs: only on Valve's own runtime
// (OpenComposite raises a fatal dialog for an interface it does not
// implement, and would not have this one), inside guarded(), behind a
// crash sentinel that disables it for the next launch if this one dies
// mid-call, and validated before it is believed -- the table's first entry
// must answer the same recommended render size the game was told.
//
// WHAT IT LOGS
//
// One line when it arms (which version answered, or which error). One line
// at the first valid centre. Then a summary every so often: how many
// frames the runtime vouched for, how many it declined, how many answers
// were malformed, and the range, mean and mean per-frame step of each
// eye's centre -- enough to tell a tracker that follows the eyes from a
// driver that publishes a constant. Never a per-frame value: the privacy
// promise of the Explorer Cam, made here for gaze.
//
// A dev instrument, [advanced], off by default. Read at launch only: the
// interface request is made once, and nothing after that is worth making
// live.
#pragma once

#include "early_session.h"  // PFN_RealGetGenericInterface
#include "openvr_min.h"

namespace edvr {

// Told the real VR_GetGenericInterface once openvr_proxy.cpp has it.
// Remembers the pointer only; the request waits for the frame loop.
void gazeProbeNoteGetter(PFN_RealGetGenericInterface get);

// Read advanced.gaze_probe. Called at hook install and on reload like every
// other reader here; the value that counts is the one at launch.
void gazeProbeConfigure();

// Per frame, from hookedWaitGetPoses after the real call returns: arms on
// the first call it can, asks every frame after, summarises on schedule.
void gazeProbeApply();

// The session's totals, once.
void gazeProbeShutdown();

}  // namespace edvr
