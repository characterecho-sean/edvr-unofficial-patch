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

// Is WxH the eye texture's own SHAPE at a plausible render scale?
//
// The size the headset is handed is not always the size the world is drawn
// at. Field-measured 2026-08-19 on a Steam install: the runtime published
// 2112x2304 an eye, the game submitted that, and every scene draw went into
// 1626x1774 -- the same shape to within 0.01%, at 77% of the width, scaled up
// on the way out. That is what supersampling below 1.0 and every upscaler
// (FSR and NIS "ultra quality" are exactly this 1.3x) do. The exact-size test
// matched only the handful of final passes, so the eye-draw count read 18 for
// a whole session and EVERY fix keyed to it was inert: Explorer Cam, the
// transition flash detector, the RemLok lines, the loading hologram and the
// witchspace star.
//
// Shape and scale only, which is deliberately not enough on its own -- a
// half-resolution post-process buffer is also the eye's shape. It says which
// targets are WORTH WATCHING; what promotes one is the draw count measured
// into it (see vscreen.cpp), because nothing but the scene draws hundreds of
// times into one target in one frame.
//
// Integer arithmetic on purpose: this is asserted from a test that links
// nothing, the same reason camera_view's grouping lives in a header.
inline bool eyeShapedAtScale(uint32_t w, uint32_t h, uint32_t eyeW, uint32_t eyeH) {
    if (!w || !h || !eyeW || !eyeH) return false;
    // Aspect, cross-multiplied rather than divided: within about 1%, which
    // the field case cleared by a factor of a hundred (0.01%) and which every
    // other target in that session's list missed by 9% or more.
    const uint64_t a = static_cast<uint64_t>(w) * eyeH;
    const uint64_t b = static_cast<uint64_t>(h) * eyeW;
    const uint64_t hi = a > b ? a : b;
    const uint64_t lo = a > b ? b : a;
    if (hi - lo > hi / 100) return false;
    // ...and a scale somebody would actually render at: 40% to 250% of the
    // submitted width. Both ends matter -- supersampling ABOVE 1.0 renders
    // large and resolves down, which starves the count the same way.
    if (static_cast<uint64_t>(w) * 5 < static_cast<uint64_t>(eyeW) * 2) return false;
    if (static_cast<uint64_t>(w) * 2 > static_cast<uint64_t>(eyeW) * 5) return false;
    return true;
}

// Is a target of this size one the headset is shown -- either the size the
// runtime published, or the size this rig turned out to render an eye at?
//
// The second half is why this exists as a shared answer instead of three
// copies of `== eyeW && == eyeH`. holo_fix and witchstar_fix identify their
// draw by an eye-sized DEPTH buffer, and on a rig with a render scale that
// buffer is the scaled size, so both fixes silently matched nothing.
// Answers false when nothing has been published and nothing measured, which
// is the same "disable yourself" answer those two already acted on.
bool vScreenIsEyeSized(uint32_t w, uint32_t h);

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

// Draws counted into a render target of this size, for the build check.
// smoke issues a known number into one and asks for it back, which is the
// only way to tell a per-draw counter from a per-binding one.
uint32_t vScreenSceneCandidateDraws(uint32_t w, uint32_t h);
bool vScreenHooksSawExecuteCommandList();

void shutdownVScreenFixes();

}  // namespace edvr
