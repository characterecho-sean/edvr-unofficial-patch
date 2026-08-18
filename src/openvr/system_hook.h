// IVRSystem_012 observation, for the terrain-culling investigation
// (frontier issue 72609). Records what the game asks about the headset's
// projection; changes nothing.
#pragma once

namespace edvr {

// Called from the VR_GetGenericInterface wrapper for every IVRSystem_*
// request. Installs the observation hook on IVRSystem_012 exactly; any other
// version is logged and left alone, because the slot ABI this hook encodes is
// specific to that generation (later ones changed GetProjectionMatrix's
// signature -- EVIDENCE 4.9).
void maybeObserveSystemInterface(void* iface, const char* interfaceVersion);

// Emits the deferred log lines: value changes, the once-per-slot register
// captures, and a cadence summary. Driven from the compositor's WaitGetPoses
// hook about once a frame, and opportunistically from the observed calls
// themselves so a session without the compositor hook still reports. Cheap
// when nothing changed.
void systemHookPeriodic();

void shutdownSystemHook();

}  // namespace edvr

// Test seam for openvr_smoke: bit 0 installed, bit 1 validated, bit 2 inert,
// bits 8-15 slot-1 (GetProjectionMatrix) call count, bits 16-23 slot-4
// (GetEyeToHeadTransform) call count, bits 24-31 slot-2 (GetProjectionRaw)
// call count, each saturated at 255.
extern "C" unsigned int edvr_selftest_system_hook(void);
