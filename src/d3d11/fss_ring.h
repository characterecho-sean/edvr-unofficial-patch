// The FSS ring draws, fed one eye's inputs for both eyes.
//
// WHERE SEVENTEEN ROUNDS LANDED (docs/fss-scanner.md, 2026-08-25): the body
// composite is byte-equivalent in both eyes -- inputs res-proven, constants
// forced, depth off, blend opaque, no discard -- yet the left eye shows
// black squares the right eye never does. The round-16/17 captures (indirect
// dispatch and draw recording, all-slot UAVs, foreign contexts, struct
// counts) found the only per-eye machinery left standing: a family of draws
// that paint the zoomed body's ring INTO each eye before the composite,
// reading PER-EYE full-eye source surfaces (colour pairs, a mask, depth-ish
// planes) that no recorded call ever writes during the build -- persistent
// surfaces prepared by machinery outside every capture window. The left
// eye's set lags; the granularity of the lag is the square.
//
// THE MOVE is the substitution pattern this repo ships twice already (the
// loading hologram, the geyser billboard): at the receiving eye's ring
// draws, bind the OTHER eye's learned SRV set, restored after each draw.
// The zoomed body sits ~9,880 units out -- parallax at that range is below
// a thousandth of a degree, so one eye's imagery is correct for both. If
// the squares die, the lagging input set is measured AND the fix has its
// shape; if they survive, the divergence rides these draws' constants, not
// their textures, and the next round knows its channel.
//
// experimental.fss_ring_feed = stock | second | first. "second" feeds the
// FIRST eye's draws the SECOND eye's inputs (one frame stale -- the lender
// draws later); "first" lends the first eye's inputs to the second,
// same-frame. Stock is off and free. Recognition rides the body-frame gate
// and three vertex-shader hashes, the fss_reveal pattern exactly.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

void fssRingConfigure(Config& cfg);

// One bool for the draw path's early-out set and the body-frame gate.
bool fssRingWantsDraws();

// Called for eye draws behind the body-frame gate. Learning (at the lending
// eye's draws) happens inside this call; true means the draw is the
// receiving eye's and a learned set is ready -- wrap it in Begin/End.
bool fssRingOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                      uint32_t instances);

// Begin: save the live SRVs, bind the learned set. End: restore, release.
void fssRingBegin(ID3D11DeviceContext* ctx);
void fssRingEnd(ID3D11DeviceContext* ctx);

// Per-frame occurrence reset, from vScreenFrameBoundary.
void fssRingFrameBoundary();

void fssRingShutdown();

}  // namespace edvr
