// IVRCompositor interception.
//
// One job: when the d3d11 side has flagged the frame in progress as drawn from
// the wrong viewpoint, do not pass it to SteamVR. Everything else is forwarded
// untouched.
//
// IVRCompositor::Submit is called once per eye with an explicit eye argument, so
// this is also the only place in the system where the per-eye boundary is
// unambiguous rather than inferred.
#pragma once

namespace edvr {

// Called from the VR_GetGenericInterface wrapper for every interface the game
// requests. Returns the pointer the game should receive: either the original, or
// the same object with a patched vtable copy. Never returns null for a non-null
// input.
void* interceptInterface(void* iface, const char* interfaceVersion);

void shutdownCompositorHook();

// The IVRCompositor versions this build knows the vtable layout of, in the
// order the table declares them (the generation the game links first).
//
// Exposed for the early handover, which has to ask the runtime for a
// compositor BEFORE the game does and therefore cannot be told which version
// to want -- it walks this list and takes the first the runtime answers.
// Reusing the one table is the point: a second copy would be a second thing
// to update when a game build moves forward, and the first to be forgotten.
//
// Submit's slot is 5 for every generation listed, and has been since
// IVRCompositor_014, but it is read from the table rather than assumed
// because the table is where that fact is written down.
size_t      knownCompositorCount();
const char* knownCompositorVersion(size_t i);   // null if i is out of range
size_t      knownCompositorSubmitSlot(size_t i);  // 0 if i is out of range

}  // namespace edvr
