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

#include "../common/vtable_hook.h"  // HookMode

namespace edvr {

class Config;

// Installs the hooks on the device's immediate context, using the hook
// mechanism the caller decided for this device -- shared with the vScreen
// hooks so the two can never split modes on the one context object. See
// device_hook.h.
void installExposureFix(ID3D11Device* device, HookMode mode);

// Reads advanced.exposure_peek -- the damping workstream's measurement
// instrument: log the exposure pass's output buffers once a second so a
// head-pitch sweep can name the float the breathing lives in. Called on
// the install path (by installExposureFix itself) and the reload path.
void exposureConfigure(Config& cfg);

// Records shader pointer -> bytecode hash, so a bound shader can be identified
// when it is dispatched.
void registerShaderHash(void* shader, uint64_t hash);

// Called once per frame from Present. The pairing of first and second eye is
// only meaningful within a frame.
void exposureFixFrameBoundary();

// Called about once a second from the frame path: verify the context-vtable
// entries this module patched still hold its thunks, and re-patch the ones
// that were re-pointed AND whose own thunks have measurably stopped being
// called WHILE A SCENE WAS RENDERING -- sceneRendered is vscreen's word that
// eye draws flowed this pass, and without it compute silence proves nothing
// (loading screens present at four figures with zero dispatches). The vouch
// tells a bypasser from a chainer, per VTableHook::reclaim. The Dispatch hook
// is this fix's only sight of the exposure pass, and a tool re-hooking it
// over EDVR (OpenXR Toolkit under OpenComposite, in the field) leaves
// detection finding nothing forever with nothing said.
void exposureFixReclaimHooks(bool sceneRendered);

// Runtime toggle, for comparing against stock behaviour without restarting.
void toggleExposureFix();
bool exposureFixEnabled();

void shutdownExposureFix();

}  // namespace edvr
