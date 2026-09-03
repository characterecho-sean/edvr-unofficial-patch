// The flashing wake marker under the speed readout, held off.
//
// WHAT IT IS (measured 2026-09-02, identified 2026-09-03)
//
// With a high wake selected, a small element under the speed readout
// pulses, in time with the target indicator's blue flash. Both are drawn
// into the SAME cockpit holo panel -- the interface surface whose aspect is
// 1.2224 -- which is why they share a rhythm.
//
// Inside that surface it is ONE draw by the textureless GUI vector shader
// 666EF0C4C616F67E, and it is a RING: 96 quads whose every vertex sits on a
// circle 0.46 to 0.49 of the way out from the panel's centre, spaced about
// eleven degrees apart. It is drawn on the frames the flash is on and not on
// the others, which is what the pulse is. Dropping it removes the flash and
// leaves the rest of the panel intact, verified on two headsets.
//
// THE COUNT IS TESSELLATION, AND THE SHAPE IS NOT
//
// The GUI subdivides its vector curves by pixel size: the same ring is 576
// indices where the panel comes out 1002x820 and 1728 where it comes out
// 2440x1996. That is why this shipped working on one headset and dead on
// the other, and why a list of numbers can never finish -- a third render
// resolution wants a third number.
//
// So the list is a head start and the ring is the identification. On a panel
// whose count is not known, one candidate draw at a time has its index slice
// and vertex buffer copied, read back four frames later, and tested: are all
// of its vertices on one thin circle about the panel's centre? Nothing else
// there is -- the clip frames reach the corners, the widgets are rectangles,
// the text is a strip. A pass adopts that count for the session; a failure
// rules it out so the next candidate gets a turn. A draw present in nearly
// every frame is refused however well it fits, because that is furniture
// rather than a pulse.
//
// The cost is one readback per candidate, only while the count is unknown,
// and nothing at all afterwards. The limitation is that the ring has to be
// ON SCREEN to be recognised, so on an unmeasured panel the first high wake
// of a session can still flash. The log says so.
//
// HOW IT WAS FOUND, because the obvious methods do not work here
//
// A census A/B finds nothing on these panels: the GUI batches its widgets
// into draws whose index counts move every frame with the content, so a
// signature never repeats. Ranking counts by how OFTEN they are drawn does
// not work either -- it picked 891 on a Q3, which is not divisible by six
// and so is not a quad list at all, and dropping it changed nothing visible.
//
// What worked was capturing EVERY vector draw into the panel across six
// frames without a wake and three with one, and keeping the geometry present
// in all three and none of the six. That left exactly one draw. Position
// answers what index counts cannot.
//
// ON by default since the field verification: the flashing is removed
// unless fix.wake_pulse = stock asks for it back. Free when off.
#pragma once

#include <d3d11.h>

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
bool wakePulseSkips(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                    uint32_t targetW, uint32_t targetH, uint32_t startIndex,
                    int baseVertex);

// The receipt: dropped draws are counted and reported, because a
// suppression that says nothing is indistinguishable from one that never
// matched.
void wakePulseReport();

// Release the staging buffers the shape test uses, if it ever ran.
void wakePulseShutdown();

}  // namespace edvr
