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

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace edvr {

void installVScreenFixes(ID3D11Device* device);

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
