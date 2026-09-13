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

}  // namespace edvr
