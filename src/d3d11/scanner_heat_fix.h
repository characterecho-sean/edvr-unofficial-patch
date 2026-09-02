// The DSS signal filter's heat map, invisible in VR -- not because it is
// faint, but because its own pixel shader discards it before the blend
// unit ever sees it, which two rounds of field testing had to find out the
// hard way.
//
// WHERE THIS COMES FROM (2026-09-02, v2's field failure)
//
// v1 (f5084cd) treated the overlay as present but swamped -- a ~7% blue
// lift measured in the eye-split dump -- and re-issued it a few extra
// times to stack that up to flat. Re-measuring the same dump with the two
// eyes properly registered (the earlier compare had read the healthy eye's
// planet against the unhealthy eye's empty sky beside it) found the
// overlay not faint in a lit eye but ABSENT, under 1% of its black-eye
// value. Re-issuing an absent draw N times adds N times nothing, which is
// exactly what the field build showed.
//
// v2 (7b77dcf) offered two hardware hypotheses: draw the overlay with
// depth testing off, or draw the shell the game stamps in front of the
// planet without depth writes -- either would explain an overlay failing
// its own depth test against a nearer stamp. Arioch ran both (bundle
// 20260902-064008, build v0.12.4-26-g7b77dcf): both engaged, the game's
// own state read back exactly as v2 expected it (depth=on
// func=GREATER_EQUAL write=zero stencil=off, rasterizer cull=FRONT
// front=CW), and NOTHING changed in either mode. The hardware
// depth-stencil unit was never the gatekeeper -- both of v2's modes are
// refuted by that read-back, not by a guess that a third mode might do
// better.
//
// A shader dump of the fill (ps `3B47A4BCE1891CC8`) settled why: it
// discards fragments ITSELF, before the blend unit ever runs, on three
// conditions entirely inside the shader and invisible to anything that
// only watches the hardware depth-stencil state --
//
//   1. a manual depth test against the eye's own linear-depth texture,
//      sampled by hand at a uv the shader computes from a constant;
//   2. a radius window around the planet centre;
//   3. a hemisphere gate against a direction constant, whose failing
//      branch is an unconditional discard regardless of the other two.
//
// What survives is coloured from two lookup textures over normalized
// altitude and blended additively (ONE, ONE). None of this touches the
// pipeline state v2 was changing, which is exactly why changing it found
// nothing.
//
// WHAT V3 DOES
//
// Transcribes the fill's disassembly verbatim into the HLSL below (cross-
// checked instruction for instruction against the fxc dump, register for
// register) and swaps it in through shader_swap.h -- resolve_probe.cpp's
// pattern: compile once, PSSetShader in Begin, restore in End, null means
// the swap failed and the fill draws stock, with shader_swap's own log
// line already saying why. advanced.scanner_heat_mode picks which of the
// three in-shader discards the compiled shader skips, so the field can
// neutralize one term at a time instead of reading a still image and
// guessing; advanced.scanner_heat_probe goes further and paints WHERE a
// term fails instead of only whether the picture changed. Only the fill is
// ever swapped -- the markers (`5FC9FC1E3B008DF1`) have a different vertex
// signature and stay exactly the game's own, though they are still
// recognised here so they keep counting for the tally and for
// advanced.scanner_heat_passes.
//
// This is a hypothesis with one shader dump and one census re-read behind
// it, not a verified fix -- see the coda docs/scanner-body.md appends for
// it. As of this writing it has not been field-tested.
//
// WHAT THE MODE WORDS DO (advanced.scanner_heat_mode)
//
//   stock      the transcription with nothing changed. Should look exactly
//              like the game -- still invisible in a lit eye. Exists to
//              prove the swap path and the transcription before anything
//              is skipped.
//   nodepth    skip discard 1, the manual depth test.
//   noradius   skip discard 2, the radius window.
//   nogate     skip discard 3, the hemisphere gate; the gated maths runs
//              regardless of which side of the gate a fragment lands on.
//   uv         take the depth sample's texture coordinate from the depth
//              texture's own dimensions instead of the constant the game
//              supplies -- in case the constant, not the test, is wrong
//              for a VR eye.
//   gain       replace the game's brightness constant with
//              advanced.scanner_heat_gain.
//   all        every one of the above at once, the default for this round:
//              answers "can the overlay be drawn at all through the swap"
//              in one look, before narrowing to which single term matters.
//
// WHAT THE PROBE WORDS DO (advanced.scanner_heat_probe, overrides the mode
// above while set -- the fill draws through a diagnostic paint instead)
//
//   why    discards nothing; paints magenta / red / green / blue / white
//          for which of four outcomes a fragment would have hit: a
//          nonsense depth sample, discard 1, discard 2, discard 3, or
//          none of the above (the game's own shader would have drawn
//          here).
//   depth  paints the sampled scene depth itself as a grey ramp, so a
//          displaced or missing disc says the depth sample is not landing
//          where it should, independent of any of the three discards.
//
// fix.scanner_heat = on | off, default off -- it changes a stock look, and
// v3 is still unverified. advanced.scanner_heat_mode and _probe choose the
// compiled variant; advanced.scanner_heat_gain feeds modes gain and all;
// advanced.scanner_heat_passes tunes the overlay's strength without a
// rebuild, independent of which term is neutralized.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

void scannerHeatConfigure(Config& cfg);

// One bool for the draw path's early-out set: neither recognizer below is
// asked unless this is true.
bool scannerHeatWants();

// True when this draw is one of the two filter shaders, the fill or a
// marker. kind gates cheaply (both are indexed-instanced 'X'); the pixel
// shader's content hash is the key, the same standard the other
// resolve-stage fixes match on. Which of the two matched is remembered for
// scannerHeatBegin -- the fill gets the swapped shader, the marker never
// does, and this is the only place that distinction is ever observed.
bool scannerHeatOnDraw(ID3D11DeviceContext* ctx, char kind);

// Around a matched draw. On a fill match: PSGetShader (saved), compile the
// replacement once via shader_swap.h and PSSetShader it in; a compile that
// failed or hasn't been tried leaves the draw alone. On a marker match:
// does nothing at all, by design -- the marker stays the game's own.
// End must run even when Begin declined, and does.
void scannerHeatBegin(ID3D11DeviceContext* ctx);
void scannerHeatEnd(ID3D11DeviceContext* ctx);

// How many EXTRA times forwardWithVerdict should re-issue the matched
// draw: a strength knob on the overlay's additive blend, independent of
// which shader variant is running. Applies to both the fill (re-issued
// under the swapped shader, inside the same bracket) and a marker
// (re-issued under whatever the game left bound, since Begin never
// touched it).
uint32_t scannerHeatExtraPasses();

// Counts one matched draw for the shutdown tally -- called once per
// original draw the kScannerHeat verdict carried, after the passes loop
// and before End, while scannerHeatBegin's fill/marker decision and its
// outcome still describe the draw being counted.
void scannerHeatNoteApplied();
void scannerHeatShutdown();

}  // namespace edvr
