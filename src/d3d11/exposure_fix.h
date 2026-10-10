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

#include "shader_registry.h"  // compatibility: legacy exposure_fix.h users get registry APIs
#include "../common/vtable_hook.h"  // HookMode

namespace edvr {

class Config;

// Installs the hooks on the device's immediate context, using the hook
// mechanism the caller decided for this device -- shared with the vScreen
// hooks so the two can never split modes on the one context object. See
// device_hook.h.
void installExposureFix(ID3D11Device* device, HookMode mode);

// THE RUNTIME'S OWN TABLE, for an instrument that has to watch it rather than
// ours. Null, and *spanOut untouched, when the exposure fix did not install.
//
// This hook goes on the context FIRST, so its vtable pointer is the table the
// runtime keeps inside the context object -- in every mode. vScreen's is the
// same table only while the hooks are in place; the moment a private mode is in
// play it is THIS hook's private buffer, which nothing outside EDVR ever
// writes. An instrument armed on that would watch a page nobody touches and
// report, truthfully and uselessly, that nothing ever happened.
void** exposureFixContextTable(size_t* spanOut);

// The exposure pass's live keys: the dispatch-skip probe, the dispatch
// experiments and the damper. Called on the install path (by
// installExposureFix itself) and the reload path.
void exposureConfigure(Config& cfg);

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

// The per-frame half, nothing vouched -- the twin of vScreenReclaimTick, on the
// other hook of the same object. Both or neither: run one at frame rate and the
// other once a second and the runtime's rewrite leaves one of them out of the
// table for up to a second while the other is already back in.
void exposureFixReclaimTick();

// Runtime toggle, for comparing against stock behaviour without restarting.
void toggleExposureFix();

// Whether the damper is configured on -- sunglare's matcher keeps running
// while it is, because the train's last-seen stamp is what scopes the
// damper to the sun.
bool exposureDampingActive();

void shutdownExposureFix();

}  // namespace edvr
