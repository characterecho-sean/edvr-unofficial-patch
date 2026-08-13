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

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace edvr {

void installVScreenFixes(ID3D11Device* device);

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

void shutdownVScreenFixes();

}  // namespace edvr
