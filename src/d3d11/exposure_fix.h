// Shares one auto-exposure result between the two eyes.
//
// Elite Dangerous computes auto-exposure separately for each eye. Because the
// eyes are a few centimetres apart, a bright source can be visible to one and
// not the other; that eye then measures a much higher peak brightness, stops
// down, and darkens its whole view while the other eye's stays bright. The
// result is one eye noticeably dimmer than the other, which is uncomfortable
// and difficult to unsee once noticed.
//
// The fix intercepts the compute dispatch that writes the exposure state and,
// after the second eye's has been computed, copies the first eye's over it.
// Both eyes then tonemap identically.
//
// Nothing is patched and nothing in the game is modified. The copy is a
// Direct3D call this DLL makes on its own behalf, and disabling it restores
// stock behaviour on the very next frame.
#pragma once

#include <d3d11.h>

#include <cstdint>

namespace edvr {

// Installs the hooks on the device's immediate context.
void installExposureFix(ID3D11Device* device);

// Records shader pointer -> bytecode hash, so a bound shader can be identified
// when it is dispatched.
void registerShaderHash(void* shader, uint64_t hash);

// Called once per frame from Present. The pairing of first and second eye is
// only meaningful within a frame.
void exposureFixFrameBoundary();

// Runtime toggle, for comparing against stock behaviour without restarting.
void toggleExposureFix();
bool exposureFixEnabled();

void shutdownExposureFix();

}  // namespace edvr
