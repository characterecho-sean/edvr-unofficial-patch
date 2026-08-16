// Moving the viewpoint, on the openvr side.
//
// WHY THIS IS ITS OWN FILE
//
// It was written inline in compositor_hook.cpp, back when this code lived in
// two repos and that file was hand-copied between them. One line did not
// survive the copy: the call that reads the offsets at install. This build
// therefore ran a whole session with headOffset still zero-initialised, and
// the feature did nothing at all for anyone who installed it. Every
// end-to-end verification in the evidence ledger had been done on the other
// build.
//
// The fork is gone (2026-08-16, one repo now) but the lesson outlives it:
// a feature that lives in its own file, called from one line, cannot be
// half-copied or half-deleted. That is worth more than the tidiness.
//
// WHAT IT DOES
//
// Elite renders from the poses IVRCompositor::WaitGetPoses returns (EVIDENCE
// 6ac.1), so translating the HMD pose translates the viewpoint -- and the game's
// own culling and object placement follow, because as far as it knows the player
// leaned. That is what makes this work where editing a camera constant buffer
// did not: those moved the rendered view and left the game's idea of the camera
// behind it.
//
// It writes to the pose array the runtime just filled, before the game reads it.
// It reads nothing from the game.
#pragma once

#include <cstdint>

#include "openvr_min.h"

namespace edvr {

// Read openvr.head_offset_* and openvr.head_yaw_degrees.
//
// MUST be called at hook install as well as on config reload. Reload-only is
// exactly the bug this file was created to make impossible: the values keep
// their C++ initialisers for the whole session, so the feature is inert until
// somebody happens to save the ini mid-flight.
void headOffsetConfigure();

// True once headOffsetConfigure has run.
//
// NOT asserted by openvr_smoke, and this comment used to claim it was. The
// assertion was written, then removed once it became clear it could not work --
// smoke LOADS the proxy as a DLL, so the DLL's copy of this module has its own
// statics and the test's copy is never configured. The claim outlived the code,
// which in a file whose subject is "a header promised something nobody
// implemented" is not a small irony.
//
// What actually enforces the install-time read is tools/check_install_reads.py,
// statically, over both repos' compositor_hook.cpp.
bool headOffsetConfigured();

// Apply the offset to the poses the runtime just returned.
//
// `err` is WaitGetPoses' own return value: on anything but success the arrays
// may be untouched or stale, and offsetting in place would accumulate -- the
// same metres added again every frame.
//
// Does nothing unless something is configured AND the gate says the player is
// on foot in the external camera. Safe to call every frame; that is the point,
// since the gate's answer changes underneath it.
void headOffsetApply(vr::EVRCompositorError err,
                     vr::TrackedDevicePose_t* renderPoses, uint32_t renderCount,
                     vr::TrackedDevicePose_t* gamePoses, uint32_t gameCount);

// Is a non-zero offset or yaw configured? For callers that want to say "this
// is set up but something else is stopping it" rather than staying silent.
bool headOffsetIsSet();

// Poses actually written this session. Zero with a configured offset means the
// gate never opened, which is a different problem from the offset being wrong.
uint32_t headOffsetAppliedCount();

}  // namespace edvr
