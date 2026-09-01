// The DSS signal filter's heat map, invisible in VR -- not because it is
// faint, but because something the game draws in front of the planet
// rejects it before it ever reaches the eye.
//
// WHERE THIS COMES FROM (2026-09-01, v1's field failure)
//
// fix.scanner_heat's first cut (f5084cd) re-issued the filter overlay a few
// extra times on the premise that it was present but swamped -- a ~7% blue
// lift measured in the eye-split dump. Arioch's field build engaged the fix
// every session, passes 2 through 6, and nothing changed. Re-measuring the
// SAME dump with the two eyes correctly registered (the per-eye projections
// are off-centre enough that the planet sits ~365 px further right in one
// eye than the other; the 7% figure had compared unregistered tiles) found
// the overlay not faint in a lit eye but ABSENT -- under 1% of its
// black-eye value. Re-issuing an absent draw N times adds N times nothing,
// which is exactly what the field build showed.
//
// The census named a candidate why. The lit eye's frame carries a
// 768-triangle shell (ps 6EF82262EB12A037, vs 41E245D488BFE83E) drawn over
// the planet with depth on, GREATER_EQUAL, WRITING depth, immediately
// before the mapped-area fills and the filter overlay -- all of which draw
// depth on, GREATER_EQUAL, NO write. Under reversed-Z the shell stamps its
// own nearer depth wherever it is nearer than the terrain; every draw after
// it then tests GREATER_EQUAL against that stamp and fails wherever the
// shell covered it. That shell is one of the four draws docs/scanner-body.md
// already documents as vanishing from one eye on alternating frames --
// normal engine parity, not a bug -- and in the frame where it is absent,
// the stamp never lands, the terrain's own depth survives, and the overlay
// passes: vivid blue over a black planet, in the eye the black-planet bug
// had already emptied -- the "blue only in the right eye" report v1's own
// commit ties to the same underlying bug.
//
// This is a hypothesis with one census and one dump behind it, not a
// verified mechanism -- see the coda docs/scanner-body.md appends for it.
// The two modes below exist to let the field distinguish "depth rejection"
// from "something else", and to settle on whichever mode actually holds.
//
// WHAT THE TWO MODES DO
//
//   overlay   the filter's two shaders (3B47A4BCE1891CC8 the heat fill,
//             5FC9FC1E3B008DF1 the markers) draw with depth testing OFF, so
//             whatever stamped depth in front of them cannot reject them --
//             regardless of which draw did the stamping. The broader claim,
//             and the one to try first.
//   shell     the shell draw stops WRITING depth (DepthWriteMask = ZERO)
//             for as long as a filter is up; the overlay is left exactly as
//             the game issues it. The narrower claim, and if the hypothesis
//             above is right, the real fix -- it restores true depth
//             semantics for everything the shell used to stamp over, not
//             the overlay alone.
//
// Both derive their depth-stencil state from the game's own, one field
// changed and the rest untouched -- resolve_probe.cpp's pattern, here for
// the same reason: a positive result has to be attributable to the ONE
// thing that changed, not to some other comparison an authored-from-
// scratch state would have gotten wrong too.
//
// fix.scanner_heat = on | off, default off -- it changes a stock look, and
// which mode is right is still an open question. advanced.scanner_heat_mode
// picks the mechanism; advanced.scanner_heat_passes tunes the overlay's
// strength without a rebuild, independent of which mode is chosen.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

void scannerHeatConfigure(Config& cfg);

// One bool for the draw path's early-out set. Covers both modes: neither
// recognizer below is asked unless this is true.
bool scannerHeatWants();

// True when this draw is one of the two heat-overlay shaders. kind gates
// cheaply (both are indexed-instanced 'X'); the pixel-shader content hash is
// the key, the same standard the other resolve-stage fixes match on. Asked
// in BOTH modes -- in shell mode nothing about this draw changes, but this
// is the only place "a filter is up" is ever observed, and a match stamps
// frameNo for scannerHeatShellOnDraw's latch.
bool scannerHeatOnDraw(ID3D11DeviceContext* ctx, char kind, uint32_t frameNo);

// True when this draw is the shell the game stamps in front of the planet,
// recognised the same way, but ONLY in shell mode and ONLY within two
// frames of the latch scannerHeatOnDraw last stamped -- the shell draws
// BEFORE the overlay within a frame, so the latch it sees here is always at
// least one frame old. Outside the scanner the latch never fires, so this
// never matches.
bool scannerHeatShellOnDraw(ID3D11DeviceContext* ctx, char kind,
                            uint32_t frameNo);

// Around a matched overlay draw: in overlay mode, binds the derived
// depth-off state; in shell mode this is a no-op, so it composes safely
// with whatever verdict forwardWithVerdict is carrying. End must run even
// when Begin declined, and does.
void scannerHeatBegin(ID3D11DeviceContext* ctx);
void scannerHeatEnd(ID3D11DeviceContext* ctx);

// Around a matched shell draw: in shell mode, binds the derived write-off
// state; in overlay mode this is a no-op. Same restore-on-every-path
// contract as the pair above.
void scannerHeatShellBegin(ID3D11DeviceContext* ctx);
void scannerHeatShellEnd(ID3D11DeviceContext* ctx);

// How many EXTRA times forwardWithVerdict should re-issue the matched
// overlay draw. Applies in either mode: it is a strength knob on the
// overlay's own additive blend, independent of which mode is keeping it
// from being rejected.
uint32_t scannerHeatExtraPasses();

// Counts an overlay draw the kScannerHeat verdict carried, for the shutdown
// tally -- in both modes, since the tally is also the "was a filter ever
// up" signal in shell mode, where the overlay draw itself is otherwise left
// alone.
void scannerHeatNoteApplied();
void scannerHeatShutdown();

}  // namespace edvr
