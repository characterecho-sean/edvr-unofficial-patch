// The RemLok helmet's edge lines, put where a helmet's edges belong.
//
// Frontier issue 69074. When the emergency helmet deploys, Elite composites a
// faint edge-line overlay onto each eye -- the SAME full-width overlay, at
// the same image positions, into both eyes. On a monitor that looks right.
// In a headset each eye's projection is asymmetric: the overlay's inner
// (nasal) line lands about forty degrees from straight ahead, seen by one
// eye only, unfusable -- the reported "something draped along your nose" --
// while the outer (temporal) line sits past fifty degrees at the lens rim
// where it is barely visible. Nothing is drawn in the wrong eye; the overlay
// is simply not stereo-aware. Users read it as eye-swapped because that is
// what it feels like from inside.
//
// HOW THE DRAW WAS FOUND, because the method is the reason to trust the
// match: a differential draw census (draw_census.h) between life-support-on
// and life-support-off in one scene and one session, cross-checked by
// position bisection (census_skip_range) and verified in the field by
// suppressing exactly this draw and watching only the lines vanish. It is:
//
//   DrawInstanced, 3 vertices, 1 instance -- a fullscreen triangle --
//   drawn once per eye onto the final per-eye target, depth unbound,
//   sampling a 1024x512 overlay texture in PS slot 0.
//
// The 1024x512 image stretched over a ~4300px eye is also why the lines
// look soft against the scene, and its position after tonemapping is why
// they read as faint grey rather than the palette's neon.
//
// THE FIX: for that one draw, substitute a scissor rectangle -- each eye
// keeps the fraction of the overlay nearest its own temple and the nasal
// remainder is clipped. The draw itself, its texture and its constants are
// untouched; the game's rasterizer state is restored immediately after.
// This reproduces what a real helmet shows each eye: your nose hides the
// inner edge. "hide" removes the overlay outright for anyone who prefers
// that; "stock" (the default until field-verified) does nothing.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// What beginPanelOverride should do with a draw it just showed us.
enum class RemlokAction : uint32_t {
    kNone = 0,   // not the overlay, or the fix is off
    kHide,       // matched, mode "hide": do not forward the draw
    kScissor,    // matched, mode "outer": wrap the draw in the per-eye clip
};

// Read fix.remlok_lines, advanced.remlok_keep_fraction and
// advanced.remlok_swap_eyes. Called on the install path and the reload path
// both, so the mode and the fraction tune live from the ini.
void remlokConfigure(Config& cfg);

// Does the fix need the per-draw path at all? False in stock mode, which is
// what keeps this module free when it is off.
bool remlokWantsDraws();

// One draw that reached an eye texture, with its bindings still current.
// Matches the overlay by shape (kind, count, instances, no depth bound,
// 1024x512 in PS slot 0) and tracks which eye this instance belongs to by
// per-frame arrival order -- the game draws the left eye's final block
// first (EVIDENCE 6y.10), and advanced.remlok_swap_eyes exists for a rig
// that proves otherwise.
RemlokAction remlokOnEyeDraw(char kind, uint32_t count, uint32_t instances);

// Around the real draw, for kScissor: set a scissor-enabled rasterizer
// state (cloned once from the game's own) and the per-eye rectangle; put
// everything back after. begin failing to engage degrades to the draw
// running untouched, and end restores only what begin actually set.
void remlokScissorBegin(ID3D11DeviceContext* ctx);
void remlokScissorEnd(ID3D11DeviceContext* ctx);

// Frame edge: the eye-parity counter resets, so a dropped frame cannot
// invert left and right for the rest of the session.
void remlokFrameBoundary();

void remlokShutdown();

}  // namespace edvr
