// Reading the constants behind the spinning sprites.
//
// THE CLASS (field-confirmed 2026-08-19 on the sun flare, loiterable at any
// star): world-positioned billboard sprites whose ORIENTATION follows the
// head -- they spin about their own centre under head roll AND yaw, and do
// not under pitch. Position is correct; the texture basis is not. The
// witchspace corona is the same family and the same artifact.
//
// WHAT DECIDES THE FIX: where the orientation comes from. If the billboard
// basis is read from constants -- the scene camera's right/up, or a
// per-sprite basis -- then substituting a world-stable basis for exactly
// those draws (the panel-distance mechanism) ends the spin. If the
// orientation is computed in the shader from screen positions (the radial
// flare construction, which the yaw-coupling hints at), no constant
// substitution can reach it, and that is worth knowing before designing
// further. Fields that TRACK the head across a captured roll/yaw sweep are
// the discriminator.
//
// THE INSTRUMENT: at each matched sprite draw, learn which constant buffer
// its vertex stage has bound; capture that buffer's contents from the same
// Map/Unmap tee the panel and camera shadows use; dump the floats -- full
// once, then only the ones that changed -- while the player rolls and yaws
// at a parked star. The offline diff against the sweep phases names the
// basis fields, if they exist. Off by default; free when off.
#pragma once

#include <cstdint>

namespace edvr {

class Config;

// Reads advanced.cb_peek. Both config paths, live.
void cbPeekConfigure(Config& cfg);
bool cbPeekEnabled();

// Every eye draw while enabled: matches the sprite family (the flare and
// corona material -- DrawIndexedInstanced, eye-sized depth in PS slot 0,
// the 1024x1024 atlas in slot 1) and learns the vertex-stage constant
// buffer the matched draw carries.
void cbPeekOnEyeDraw(char kind, uint32_t count, uint32_t instances);

// The buffer being watched, or null. Compared by the Map hook, never
// dereferenced there.
void* cbPeekTarget();

// The watched buffer's freshly-written contents, from the Unmap tee, read
// before the memory is returned to the driver. Throttled and line-capped
// internally.
void cbPeekCapture(const void* data, uint32_t bytes);

}  // namespace edvr
