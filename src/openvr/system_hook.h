// IVRSystem_012 observation and, since phase 1, the cull guard -- the
// mitigation for frontier issue 72609 (terrain quads culled at the FOV edges
// over planets). Observation records what the game asks about the headset's
// projection; the guard, when armed in edvr.ini, answers those questions
// with a WIDER frustum than the headset shows and has the compositor crop
// the image back, so terrain the culler drops at the visible edge is drawn.
#pragma once

#include "openvr_min.h"

namespace edvr {

// Called from the VR_GetGenericInterface wrapper for every IVRSystem_*
// request. Installs on IVRSystem_012 exactly; any other version is logged
// and left alone, because the slot ABI this hook encodes is that
// generation's (later ones changed GetProjectionMatrix's signature --
// EVIDENCE 4.9).
void maybeObserveSystemInterface(void* iface, const char* interfaceVersion);

// Re-reads the guard's config (fix.cull_guard, fix.cull_guard_percent).
// Called at the system hook's own install, from the compositor's install
// path, and from the once-a-second reload poll -- the margin is tuned from
// inside a headset, so it must be live. Whether the MATRIX half of the lie
// can run at all is decided once, at install (the member-shaped receiver
// either replaced the slot or did not); this only moves the mode within
// what that decision allows.
void systemHookConfigure();

// The frame boundary, called from the compositor's WaitGetPoses hook. The
// lie switches on and off ONLY here, so every projection answer within one
// frame -- raw, matrix, and the submit crop at its end -- tells one story.
// In a process with no compositor hook (the test harness), periodic's
// two-second fallback promotes instead.
void systemHookFrameBoundary();

// Deferred log emission: value changes, register captures, cadence summary.
// Driven from the frame boundary and opportunistically from the observed
// calls themselves. Cheap when nothing changed.
void systemHookPeriodic();

// The submit-side half of the guard, in three pieces the compositor hook
// composes. The observation notes in hookedSubmit must keep seeing the
// ORIGINAL texture and bounds throughout: the d3d11 half matches eye
// textures by the size those notes publish, and the game's render target
// has not changed size.
//
// True when the lie is live for this eye, with out[4] = the crop fractions
// {left, top, right, bottom} of the true frustum within the reported one.
bool systemHookCropFractions(vr::EVREye eye, float out[4]);

// True when the guard should crop by COPYING the region into an EDVR-owned
// texture (guard_crop.h) -- the default, after narrowed bounds were ignored
// by OpenComposite in the field (2026-08-18, hall of mirrors). False =
// advanced.cull_guard_submit is "bounds": narrow the submitted bounds
// instead, kept for runtimes whose bounds handling is known good.
bool systemHookSubmitCopyMode();

// Bounds mode's implementation: the caller's bounds (or the whole texture,
// when null) narrowed to the region holding the TRUE frustum. Returns false
// when the guard is not live; the caller then submits untouched.
bool systemHookCropBounds(vr::EVREye eye, const vr::VRTextureBounds_t* in,
                          vr::VRTextureBounds_t* out);

// The compositor hook's lever for a submit-side failure (the crop copy
// refused): the guard goes inert LOUDLY and the lie ends at the next frame
// boundary -- one mismatched frame at worst, instead of a session of them.
void systemHookGuardStandDown(const char* why);

void shutdownSystemHook();

}  // namespace edvr

// Test seam for openvr_smoke: bit 0 installed, bit 1 validated, bit 2 inert,
// bits 8-15 slot-1 (GetProjectionMatrix) call count, bits 16-23 slot-4
// (GetEyeToHeadTransform) call count, bits 24-31 slot-2 (GetProjectionRaw)
// call count, each saturated at 255.
extern "C" unsigned int edvr_selftest_system_hook(void);

// Test seam for the guard: returns 0 if the lie is not live for this eye;
// otherwise 1, with out[4] = the crop fractions {left, top, right, bottom}
// of the true frustum within the reported one.
extern "C" unsigned int edvr_selftest_cull_guard(int eye, float out[4]);
