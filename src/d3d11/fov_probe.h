// Finding the game's own projection terms, by watching which floats ramp.
//
// WHY: the witchspace star pin converts head rotation to pixels through the
// HEADSET's projection, and the field measured that being wrong by about a
// factor of two that DRIFTS across a jump -- the signature of the tunnel
// pass rendering with its own wider, animated FOV. No constant gain can be
// right against an animated projection; the true divisor has to come from
// the projection the game is actually using, per frame. More is behind the
// same door: the tunnel and the entry streaks are further head-locked
// layers, and every screen-composited sprite that "rotates with your head"
// needs this same live term to be stabilised properly.
//
// WHERE TO LOOK: the scene camera constant buffer -- 5,376 bytes the
// transition-flash fix already recognises by size and reads every frame.
// Its float 1100 is the camera's position (6k, 6s); the projection terms
// have never been hunted. This probe is the hunt: while a jump tunnel is
// on screen (the journal window), it samples the whole buffer twice a
// second and reports the floats that RAMP -- change smoothly, same
// direction, sample after sample -- which is what an animated FOV does and
// what positions (jumpy) and constants (still) do not.
//
// A diagnostic, off by default, costing one comparison pass per sample
// while enabled and nothing at all otherwise.
#pragma once

#include <cstdint>

namespace edvr {

class Config;

// Retired instrument (no keys read; always off): the witchspace arc is closed.
void fovProbeConfigure(Config& cfg);

// The camera buffer's contents, as the game just wrote them -- called from
// the same Unmap tee that feeds the flash detector. Samples on its own
// clock; cheap early-out when disabled or between samples.
void fovProbeObserve(const void* data, uint32_t bytes);

}  // namespace edvr
