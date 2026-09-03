// The flashing wake marker under the speed readout, held off.
//
// WHAT IT IS (measured 2026-09-02)
//
// With a high wake selected, a small element under the speed readout
// pulses, in time with the target indicator's blue flash. Both are drawn
// into the SAME cockpit holo panel -- the interface surface measuring
// 0.4498 x 0.3726 of the render resolution (2440x1996 on a 5424x5356 eye,
// 1952x1997 on a 4340x4284 one) -- which is why they share a rhythm.
//
// Inside that surface it is one draw: the textureless GUI vector shader
// 666EF0C4C616F67E, 1728 indices, 288 quads. It is present in only SOME
// frames -- measured 0 of 3 with no wake, 1 of 3 and 2 of 3 with one -- and
// that IS the pulse: the element is drawn on the frames the flash is on and
// not on the others. Suppressing that draw removes the flash and leaves the
// rest of the panel intact, verified in the field.
//
// HOW IT WAS FOUND, because the obvious method does not work here
//
// A census A/B finds nothing on these panels, in either direction. The GUI
// batches its widgets into a handful of draws whose index counts change
// every frame with the content -- the speed digits alone guarantee it -- so
// a signature never repeats across frames and "in every frame of one and no
// frame of the other" can never be satisfied. Restricting the comparison to
// one SURFACE cut the noise enough for its stable shapes to be compared,
// and that is what named this draw. For the cockpit HUD, suppression is the
// instrument; differential capture is not.
//
// THE INDEX COUNT IS CONTENT-DEPENDENT, and that is this fix's weak point.
// 1728 is what this widget's batch measured on game build 332753; another
// build, or a different panel layout, can change it. So it is a setting,
// the log says how many draws were dropped, and a count of zero means the
// number needs re-deriving rather than that the fix is broken. The surface
// is matched by RATIO instead, which is session-proof -- a literal size is
// not, and reusing a stale one cost a day of this investigation.
//
// ON by default since the field verification: the flashing is removed
// unless fix.wake_pulse = stock asks for it back. Free when off.
#pragma once

#include <cstdint>

namespace edvr {

class Config;

// Reads fix.wake_pulse (stock | off) and
// advanced.wake_pulse_indices. Install and reload; live.
void wakePulseConfigure(Config& cfg);

// False in stock mode, which keeps the offscreen draw path free.
bool wakePulseWantsDraws();

// Is this offscreen draw the pulse? Called with the draw's shape and the
// size of the target it is landing in, from the offscreen branch that
// already resolves both. True means drop it.
bool wakePulseSkips(char kind, uint32_t count, uint32_t targetW,
                    uint32_t targetH);

// The receipt: dropped draws are counted and reported, because a
// suppression that says nothing is indistinguishable from one that never
// matched.
void wakePulseReport();

}  // namespace edvr
