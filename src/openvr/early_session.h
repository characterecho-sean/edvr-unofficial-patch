// The early VR handover.
//
// OpenComposite starts an OpenXR session before it knows the application's
// graphics API, then tears that session down and rebuilds it on the first
// compositor call that carries a texture. The teardown is
// DrvOpenXR::ShutdownSession, which asks the runtime to exit and then polls:
//
//     xrRequestExitSession(...)
//     while (GetSessionState() != XR_SESSION_STATE_EXITING && count++ < 10) {
//         Sleep(250); PumpEvents();
//     }
//
// On the field rig that loop runs all ten times and gives up -- the runtime
// never reaches EXITING -- so the rebuild costs the full 2.5 s every launch.
// Measured 2026-08-28, Pimax Crystal Super behind OpenComposite 1.0.1539:
//
//     41.474  the game creates its D3D11 device
//     42.637  OpenComposite VR_InitInternal2
//     42.671  first OpenXR session started
//     45.128  the game is handed IVRCompositor_014
//     45.130  "Recreating OpenXR session for application graphics API"
//     45.130..47.383   ten x 250 ms waits
//     47.643  second session started
//     47.816  the game's first real eye submit
//
// The 2.5 s therefore lands INSIDE the game's first compositor call, on the
// game's own thread, while the intro movie's decoder is already running. The
// movie's first frame does not reach the eye until 47.8, by which time it has
// been playing to nobody for seconds. That is the "several seconds are cut
// off" the field report describes, and the freeze with it.
//
// This file does the rebuild EARLY and deliberately, at the first interface
// the game asks for -- 42.67, two and a half seconds before the game wants
// the compositor. The cost does not go away; it moves to a point where the
// game is still loading and nothing is on screen to interrupt. By the time
// the game asks for IVRCompositor the session is already the right one, so
// CheckOrInitCompositors finds its compositors present and returns.
//
// Nothing here is Elite-specific and nothing patches OpenComposite. It makes
// one ordinary OpenVR call, early, with a texture on the game's own device --
// the same call the game makes later, which is exactly why it works.
#pragma once

#include "openvr_min.h"

namespace edvr {

// Run the handover, once, if the config asks for it.
//
// Call from the earliest point the openvr proxy is given control -- the first
// VR_GetGenericInterface. Safe to call repeatedly: everything after the first
// call returns immediately.
//
// getGenericInterface is the REAL VR_GetGenericInterface, passed in rather
// than looked up because openvr_proxy.cpp owns that pointer and this file
// must never reach around it to the export it is itself standing in for.
typedef void* (__cdecl* PFN_RealGetGenericInterface)(const char* version,
                                                     vr::EVRInitError* error);
void earlySessionRun(PFN_RealGetGenericInterface getGenericInterface);

}  // namespace edvr
