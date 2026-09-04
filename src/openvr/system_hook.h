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

// The size-adoption handshake for the guard's two-stage go-live. While the
// guard is in stage 1 (render targets asked bigger, projections still true)
// the compositor probes each submitted texture's size and reports it here;
// stage 2 -- the actual lie and crop -- waits until both eyes' submissions
// arrive at the inflated size, so the CROPPED submission always lands at
// exactly the size the session already established. Both field failures
// (ignored bounds, then the parallelogram) were the transport mishandling a
// submission shape it had not served before; this guarantees it never sees
// one.
bool systemHookSizeProbeWanted();
void systemHookNoteSubmittedSize(vr::EVREye eye, uint32_t w, uint32_t h);

// The exact pixel size the crop must land on for this eye (the canonical
// submission size frozen when stage 2 engaged), or false when no snap is
// required (boundary-less test processes).
bool systemHookCropTarget(vr::EVREye eye, uint32_t* w, uint32_t* h);

// The runtime's recommended per-eye render size, as last answered on slot
// 0 -- the TRUTH, before any guard inflation. False until the game has
// asked (or the hook never installed). The supersample resolve keys on the
// ratio between this and what the game actually submits
// (supersample_resolve.h); nothing else here needs it.
bool systemHookRecommendedSize(uint32_t* w, uint32_t* h);

// The size the game renders each eye at when Elite's HMD Quality is 1.0:
// the runtime's recommendation, or the guard's widened answer while its
// size lie stands. What a lowered HMD Quality is a fraction OF, and so
// the size an upscaling pass brings the frame back to. False until seen.
bool systemHookUnitQualitySize(uint32_t* w, uint32_t* h);

// The temporal pass's jitter (temporal_aa.h): a shift of every tangent the
// game is told for the coming frame -- l and r by dx, t and b by dy -- set
// at the frame boundary and held for the whole frame, the guard's own
// consistency rule. live = false clears it. Applied in BOTH the raw thunk
// and the matrix receiver, or not at all: see systemHookJitterAvailable.
void systemHookSetJitter(float dx, float dy, bool live);

// Whether a jitter can be told to the game at all: the member-shaped
// matrix receiver is installed (decided at launch -- the guard armed, or
// fix.temporal_aa on) and both eyes' matrices matched the tangent formula.
// Without it the raw thunk alone would shift the tangents while the matrix
// stayed put, and the game would render through one and be un-jittered
// through the other.
bool systemHookJitterAvailable();

// The same question with its third answer: +1 available, -1 never this
// session (no receiver, an inert hook, or a runtime whose matrix failed
// the tangent-formula check), 0 not yet known -- the receiver is in place
// and the game has not asked for both eyes' matrices yet, which is where
// the check happens. A caller that wants to say "cannot" waits for -1.
int systemHookJitterVerdict();

// How many times the game has asked for its projection (GetProjectionRaw
// and GetProjectionMatrix together) since launch. The temporal pass reads
// it at the frame boundary and at the first submit to learn WHEN the game
// reads the projection relative to the boundary its jitter is set at.
uint32_t systemHookProjectionReads();

// The tangents the game is being told THIS frame, jitter excluded: the lie
// under a live guard, the truth otherwise. False until that eye has been
// seen.
bool systemHookEffectiveTangents(vr::EVREye eye, float out[4]);

// The IVRSystem the game was handed, or null.
//
// Only ever non-null for IVRSystem_012: maybeObserveSystemInterface refuses
// every other version outright, so a caller holding this also holds this
// build's field-validated slot map for it (slots 0/1/2/4 are checked live).
// That refusal is the safety -- a pointer to some other generation would be
// a vtable this build cannot index.
// The eye's offset from the head (GetEyeToHeadTransform, the whole 3x4,
// row-major), asked of the runtime once through the original entry and
// kept: the translation the depth reprojection needs per eye. False until
// the interface is observed or when the answer is not a rigid transform.
bool systemHookEyeToHead(vr::EVREye eye, float out[12]);

// The near and far planes the game names when it asks for its projection
// matrix (0.025 and 50000 on every session so far), for reading its
// reversed-Z depth in metres. False until the first such call.
bool systemHookNearFar(float* nearZ, float* farZ);

void* systemInterfaceV012();

// How many vtable slots that interface was measured to have -- VTableHook's
// executablePrefix, the count of entries that looked like real code. 0 if
// nothing is hooked.
//
// A caller reaching for a slot must check it against this. system_hook only
// ever validated ">4" for its own use, so a higher slot is unproven until
// asked about individually.
size_t systemInterfacePrefixV012();

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
