// The flat panel Elite renders on-foot VR onto.
//
// On foot the world is not drawn in stereo. It is rendered once, flat, and
// shown on a floating screen inside your headset. Two things about how that
// screen is presented can be improved without touching the image itself:
//
//   the void around it   Elite clears both eye textures to a dark grey and
//                        draws the panel into that, so the screen floats in
//                        haze. On an OLED that is lit pixels where there
//                        should be none.
//
//   the panel's distance How far away the screen sits. Fixed by the game;
//                        adjustable here, for anyone whose comfort differs.
//
// Neither changes the panel's resolution, which is what makes it blurry. That
// turned out not to be fixable from here -- the game's render resolution is set
// in 29 different places and its shaders are told about it separately.
#pragma once

#include <cstdint>

#include "../common/vtable_hook.h"  // HookMode

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace edvr {

// Above this many draws into the eye textures in one frame, a SCENE is being
// rendered rather than a menu.
//
// Measured, not chosen: menu-only sessions peak at 20 and 22 (2026-08-14
// 15:48, 2026-08-15 16:21, neither reaching LoadGame) and the flash detector's
// own validation saw 0-to-22 over 300 menu frames, while gameplay clears 100
// even in sessions quit seconds after loading in (peaks of 119 and 126)
// against session peaks of 975 on a Quest 3 and 1074 on a Pimax.
//
// It lives HERE because the count is this module's -- it is incremented in
// beginPanelOverride and handed out at the frame boundary -- and because the
// alternative is a fourth copy of one measurement. camera_view kept its own
// (kMenuEyeDraws) and its comment already said what that costs: "a third
// number for it would be a third thing to re-measure". glitch_frame's
// minEyeDraws is deliberately still its own, being a per-fix tunable rather
// than this fact.
constexpr uint32_t kSceneEyeDraws = 100;

// Installs the context hooks using the mechanism the caller decided for this
// device -- shared with the exposure hooks so the two never split modes on
// the one context. See device_hook.h.
void installVScreenFixes(ID3D11Device* device, HookMode mode);

// The size the on-foot panel actually renders at, once that is settled.
//
// This is how the panel composite is recognised -- it is the draw into an eye
// texture that SAMPLES something of exactly this size -- so a wrong answer
// silently disables the panel distance fix.
//
// It has to come from what the resolution patch DID, not from what edvr.ini
// asked for, and those differ in both directions. Taking it from config alone
// meant 2560x1440 -- a legal setting that gets applied -- left this at the
// stock size, because 1440 is under an unrelated 2048 threshold; and a patch
// that REFUSED left it claiming 4K while the panel really rendered 1920x1080.
// Both end with the fix quietly doing nothing.
void vScreenSetPanelSize(uint32_t width, uint32_t height);

// Re-read the settings that are documented as changeable while the game runs.
//
// Both black_void and panel_distance say so in the README and in edvr.ini, and
// neither was ever re-read: the values were taken once at install and nothing
// called reloadIfChanged anywhere in the DLL. Editing the file mid-session did
// nothing, which is indistinguishable from the fix being broken -- and that is
// what it was reported as.
void vScreenRefreshConfig();

// Called once per frame. The panel composite is identified partly by how many
// draws reach the eye textures in a frame, which is only knowable at the end
// of one.
void vScreenFrameBoundary();

// Called about once a second from the frame path: verify the context-vtable
// entries this module patched still hold its thunks, and re-patch the ones
// that were re-pointed AND whose own thunks have measurably stopped being
// called -- the vouch that tells a bypasser (re-patch, both run) from a
// chainer (leave alone, or the chain loops). A bypasser doing exactly that --
// OpenXR Toolkit under OpenComposite, resolving its "original" pointers from
// a clean table -- is how every fix in this file went silent in the field
// while the log looked half-alive. See VTableHook::reclaim.
//
// Returns whether any eye draws were counted since the previous pass -- the
// scene evidence the exposure fix's own vouches require, because its compute
// slots go legitimately silent through loading screens and silence there
// proves nothing. False when this module is not installed, which correctly
// leaves the exposure fix detection-only.
bool vScreenReclaimHooks();

// Did the ClearState and ExecuteCommandList hooks actually run?
//
// For the build check only. Their vtable slots were counted from declaration
// order rather than measured, and ClearState's neighbour is FinishCommandList --
// so smoke calls both and asks. Calling them and merely observing that rendering
// still works cannot catch a miscount: every plausible off-by-one lands on a
// method the test never invokes.
bool vScreenHooksSawClearState();
bool vScreenHooksSawExecuteCommandList();

void shutdownVScreenFixes();

}  // namespace edvr
