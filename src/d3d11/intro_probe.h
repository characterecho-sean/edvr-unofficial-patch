// The startup sequence, timed and named -- the one stretch of a session no
// existing instrument can be aimed at.
//
// THE DEFECT THIS EXISTS FOR. Launched in VR, Elite plays its intro movie
// full-view at the mirror window's resolution, freezes for a second or two,
// and then continues the same movie in a small head-locked rectangle. Most
// players press Escape through it. The wanted behaviour is the one the game
// already has minutes later: a black void with a panel floating in it
// (docs/intro-video.md).
//
// WHY A NEW INSTRUMENT. Everything else here is armed by a keypress, by a
// journal event, or by a draw into a target of a size somebody already knows.
// The intro has none of those:
//
//   * The census key (draw_census.h) is armed by hand, and the phase that
//     matters most -- the freeze -- is over about two seconds after the
//     window appears. Human reaction lands after it.
//   * advanced.census_auto fires on a draw into a NAMED size after two quiet
//     seconds. Nobody knows the size yet, and the video's target is drawn
//     into every frame from the first one, so the quiet spell never arrives.
//   * The eye-texture gate every other module sits behind cannot answer
//     during the intro's first phase at all: one eye's size is published by
//     the openvr half at the compositor's first Submit, which in a measured
//     session was 5.5 seconds after the d3d11 proxy attached -- AFTER the
//     freeze. Before that, every draw in the frame is "offscreen".
//
// So this records the one thing that is knowable with no prior model: WHERE
// the frame's draws are going, WHEN each frame happened, and what the game
// cleared. Three phases with a stall between them is a shape; a shape names
// the targets; a named target is what the census and the quad probe can then
// be aimed at.
//
// WHAT IT RECORDS, per frame, while on:
//
//   composition  the render targets that received draws, with counts, eye-
//                sized ones marked. Logged only when the SHAPE changes --
//                the sizes, or a count's power-of-two class -- so a startup
//                running at 271 fps (measured) writes a handful of lines
//                rather than thousands.
//   stalls       any frame longer than kStallMs, with when it happened. The
//                freeze is the measurement; a Present gap is where it shows.
//   clears       every distinct target-size-and-colour pair the game asks
//                for, read BEFORE the black void fix substitutes -- so the
//                line says what the GAME wanted, which is what decides
//                whether the intro's void is already grey-to-black material.
//
// It changes nothing. It is off by default, free when off (the draw path
// does not call in), and stands itself down after kWindowMs or kMaxLines so
// a session left with it on does not grow a log all evening.
#pragma once

#include <cstdint>

namespace edvr {

class Config;

// Reads advanced.intro_probe. Install and reload; live, though turning it on
// mid-session records whatever is happening then rather than the startup it
// is named for.
void introProbeConfigure(Config& cfg);

// Is the probe recording? False when off and false once the window has
// closed, which keeps it out of the draw path for the rest of the session.
bool introProbeWants();

// One draw, with the size of the target it lands in (0x0 when the binding
// could not be resolved) and whether that target is one the headset is shown.
void introProbeOnDraw(uint32_t targetW, uint32_t targetH, bool eyeSized);

// One ClearRenderTargetView, called with the colour the GAME asked for.
void introProbeOnClear(uint32_t targetW, uint32_t targetH, const float rgba[4]);

// The frame edge: close the frame's composition, emit a line if the shape
// moved or the frame stalled, and retire the probe when its window is spent.
void introProbeFrameBoundary(uint32_t frameNo);

}  // namespace edvr
