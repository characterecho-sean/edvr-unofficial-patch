// The DSS signal heat map, redrawn so it reads in VR the way it does on a
// flat monitor.
//
// WHERE THIS COMES FROM (2026-09-01, out of the black-planet hunt)
//
// With a signal filter selected in the Detailed Surface Scanner (fumaroles,
// biological, and so on) the game shades where those signals are in blue.
// On a flat screen that overlay is bold. In VR it is all but invisible --
// measured at a ~7% blue lift over the disc in the healthy eye's own
// pixels, against the saturated blue the same planet shows on flat. It was
// only ever noticeable in the pre-fix black eye, where the missing planet
// left the overlay the one thing lit; that is the "blue only in the right
// eye" report, and it was the bug flattering the overlay, not the overlay
// failing per eye.
//
// The overlay draws additively (ONE, ONE, ADD) into each eye's lit buffer,
// tests depth GEQUAL, writes neither depth nor stencil. Its HDR
// contribution is small and the VR scene's lit values swamp it before the
// tonemap; flat's exposure regime leaves it standing. The blend being
// additive is the whole opportunity: re-issuing the same draw adds its
// colour again, so N extra passes make the overlay N+1 times as strong with
// no other change -- it can only add light where the overlay already is,
// never darken or spread.
//
// WHICH DRAWS
//
// Two pixel shaders, and only two, appear ONLY when a signal filter is
// selected -- absent from every unfiltered DSS census and from normal
// space alike:
//
//   3B47A4BCE1891CC8   the filter's heat fill (~1800 triangles of coverage)
//   5FC9FC1E3B008DF1   the filter's signal markers (nine small quads)
//
// The neighbouring blue-fill shaders (the always-present mapped-area
// shading) are deliberately NOT touched: they also draw in normal space,
// where boosting them would tint the terrain. These two are the filter
// overlay and nothing else.
//
// fix.scanner_heat = on | off, default off -- it changes a stock look and
// its strength wants calibrating, so it is opt-in until dialled in.
// advanced.scanner_heat_passes tunes the strength without a rebuild.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

void scannerHeatConfigure(Config& cfg);

// One bool for the draw path's early-out set.
bool scannerHeatWants();

// True when this draw is one of the two heat-overlay shaders. kind gates
// cheaply (both are indexed-instanced 'X'); the pixel-shader content hash is
// the key, the same standard the other resolve-stage fixes match on.
bool scannerHeatOnDraw(ID3D11DeviceContext* ctx, char kind);

// How many EXTRA times forwardWithVerdict should re-issue the matched draw.
uint32_t scannerHeatExtraPasses();

// Counts the boost for the log, and prints the tally when cleared or at
// exit -- a session that boosted nothing never had a filter up.
void scannerHeatNoteApplied();
void scannerHeatShutdown();

}  // namespace edvr
