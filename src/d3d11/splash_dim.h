// The splash screen dimmed under the loader's dialogs -- the scrim's job,
// done where a headset wants it done.
//
// WHY. On a monitor, the game's full-view scrim and "dim the screen" are
// the same thing, because the view IS the screen. In a headset they are
// not: the scrim tints the whole panorama, which is the defect
// fix.loading_panel withholds -- but the DESIGN behind it, the splash
// stepping back while a dialog talks, is worth keeping. So while the
// withhold is active, the splash screen's own composite -- the quad that
// lifts the still (or the intro movie) into each eye -- is drawn a second
// time with a solid black pixel shader at the scrim's own measured alpha
// (0x66, exactly 0.4). Same geometry, same placement constants, same
// draw call re-issued: the tint lands exactly on the screen, flat as the
// screen is, and nowhere else.
//
// WHEN. Exactly the frames the game wanted its scrim: fix.loading_panel's
// withhold marks each frame it swallows the scrim, and the screen
// composites draw later in the same frame, so the dim follows the game's
// own schedule with zero lag -- white-text phase, dialogs, gaps, all of
// it, and never after the intro retires.
//
// HOW LITTLE. Begin binds a compiled-once pixel shader (returning
// 0,0,0,0.4) and a src-alpha blend over the still-bound state of the
// composite that JUST drew; the caller re-issues the same draw; End puts
// the game's shader and blend back. Every failure -- no compiler, no
// blend state, an RTV that is not an eye -- stands down and the splash
// simply stays undimmed.
#pragma once

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Reads fix.loading_splash_dim (on | off). Install and reload; live.
void splashDimConfigure(Config& cfg);

// Arm the dim for one re-issue of the screen composite that just drew:
// true means the dark shader and blend are bound and the caller must
// re-issue the draw and then call splashDimEnd. False means nothing was
// touched -- off, no withhold this frame, not an eye target, or the
// machinery failed and said so.
bool splashDimBegin(ID3D11DeviceContext* ctx);

// Restore the game's pixel shader and blend state. Always paired with a
// true splashDimBegin.
void splashDimEnd(ID3D11DeviceContext* ctx);

void splashDimShutdown();

}  // namespace edvr
