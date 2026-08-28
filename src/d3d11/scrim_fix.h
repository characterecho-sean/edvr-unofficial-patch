// The loading screen's dimming wash, held off the artwork behind it.
//
// THE DEFECT, as the field described it: while the loader's "PREPARING
// SHADERS" dialog is up, a large curved surface darkens everything behind
// it -- a modal scrim, the ordinary UI pattern where a dialog dims its
// background so it reads. On a monitor it is unremarkable. In a headset it
// covers most of the field of view, and with the menu backdrop fix showing
// the good splash art underneath (backdrop_fix.h) what it dims is worth
// looking at.
//
// HOW IT WAS FOUND, because five attempts failed first and the reason is
// instructive. The wash is not a draw of its own:
//
//   * skipping every draw INTO the UI buffer -- 13,754 of them -- removed
//     the whole interface and left the wash standing;
//   * the buffer's clear was measured at r=0 g=0 b=0 a=0, so it is not the
//     clear either;
//   * it is not the vScreen panel: no 5120x2880 target exists during the
//     loader at all;
//   * EDVR's own panel curvature is not bending it -- at 0.0 it still curved,
//     so the curve is the game's geometry.
//
// Every one of those was a signature read out of a single census and bet on.
// What settled it was the instrument built for the job: two censuses in one
// session, one with the wash and one without, through
// tools/diff_draw_census.py. The diff is the whole finding:
//
//   ADDED    X n=5760  vs=A888D51024D9798E  samples=[16x16 BC1, 4259x2395]
//   REMOVED  X n=5760  vs=4EF6DDB075A927FA  samples=[4259x2395]
//
// The same curved mesh, the same index count, a different vertex shader and
// ONE EXTRA TEXTURE. When the wash appears the draw gains a sixteen-pixel BC1
// in slot 0, stretched across the whole surface. The scrim is a modulation
// term on the draw that composites the interface -- which is exactly why no
// suppression, clear or panel hypothesis could ever have found it.
//
// WHAT THE SHADER ACTUALLY DOES, read from its own bytecode rather than
// guessed at -- the step that should have come before the first attempt.
// ps 9107E72CB016CC02 samples the interface texture, blurs it over an
// EIGHT-TAP loop along a view-dependent vector (the "squiggly lines" the
// field reported), desaturates it towards luminance, tints it by cb2[8] and
// cb2[9], and then:
//
//   mad r0.xyzw, r2.xyzw, r0.xxxx, r3.xyzw   ; r0.x, r0.y are the t0 samples
//   add r0.xyzw, r0.xyzw, r1.xyzw            ; + the sharp interface on top
//
// The two samples of the 16x16 MULTIPLY that whole layer. So the wash is a
// frosted-glass pass whose strength this texture scales.
//
// THE FIX is holo_fix's, applied to slot 0 instead of slot 1: for that one
// matched draw, substitute a uniform 1x1 texture and put the game's back
// immediately after. At level 0 both terms collapse, r0 becomes r1 -- the
// sharp interface alone -- and the shader's own discard (every channel below
// 5/255) throws the empty area away. That is what the no-wash variant
// 85565E9261812E2F does with its own discard, arrived at from the other end.
//
// This shipped at 255 first, on the reasoning that white neutralises a
// multiply. White neutralises a multiplied COLOUR; here the term multiplies
// a LAYER, so 255 turned the wash to full strength and the field reported no
// improvement. Reading the disassembly took minutes and would have saved
// several flights: the guess was cheap to make and expensive to test.
//
// WHAT THIS OVERRIDES. Frontier put the wash there so a dialog stays legible
// against arbitrary backgrounds. Turning it off is a preference, not a bug
// fix: the modal keeps its own opaque black panel, so the text stays
// readable, but the judgement being overridden is a real one. Off by default.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Reads fix.loading_dim (stock | off) and advanced.loading_dim_level.
// Both paths, install and reload; both live.
void scrimConfigure(Config& cfg);

// False in stock mode, which keeps the per-draw path free when off.
bool scrimWantsDraws();

// Is this eye draw the loader's UI composite carrying the wash? Matched by
// what it SAMPLES, not by shader hash: A888D51024D9798E is the engine's
// general world-quad pipeline and matches half the game (the FSS panel work
// and the loading screen's text quad both learned that the hard way). The
// discriminator is the pair -- a 16x16 BC1 in slot 0, which nothing else
// stretches across a mesh, and a large interface surface in slot 1.
bool scrimOnEyeDraw(char kind, uint32_t count, uint32_t instances);

// Around the real draw: bind the uniform into PS slot 0, restore the game's
// texture after. End is safe to call when Begin did nothing.
void scrimBegin(ID3D11DeviceContext* ctx);
void scrimEnd(ID3D11DeviceContext* ctx);

void scrimShutdown();

}  // namespace edvr
