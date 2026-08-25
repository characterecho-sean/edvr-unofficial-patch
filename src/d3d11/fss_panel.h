// The FSS panel at the player's chosen distance.
//
// WHAT (docs/fss-panel.md, 2026-08-25): the scanner's screen in VR is a
// quad instanced into the world through Elite's general mesh pipeline --
// a position-only depth prepass (vs B018D143700AB803) and a textured pass
// (vs A888D51024D9798E), chrome sampled at PS slot 1, placement from a
// structured-buffer instance record, everything camera-relative. None of
// the on-foot panel machinery can reach it: its transform lives in
// instance data, not in a substitutable constant buffer.
//
// THE FIX is the particle billboard's mechanism: replacement vertex
// shaders (fss_panel_vs.h), mechanical transcriptions of the game's own
// pair with one change -- the camera-relative world position is scaled by
// a factor before projection, which moves the screen along the line of
// sight without changing its angular size. Both shaders carry the factor
// or the prepass and the colour pass disagree about depth and the quad
// fails its own depth test; recognition therefore swaps BOTH, by vertex
// shader hash, and a factor change recompiles the pair.
//
// fix.fss_panel_distance: 0 (the default) inherits fix.panel_distance, so
// one setting places both screens; an explicit value overrides; anything
// within a percent of 1.0 disables the swap outright and the game draws
// stock. Hash-pinned recognition means a game update that rebuilds these
// shaders leaves the fix silently inert -- the totals note says when it
// never engaged, which is that case's name.
//
// THE HASH IS NOT THE SCANNER (field, 2026-08-25, one session old): this
// pair is the engine's general world-quad pipeline, and matching it by
// hash alone moved the LOADING SCREEN's text quad. vscreen therefore
// gates the recognition on the scanner's body layer having drawn within
// the last two frames -- drawing that layer is the one thing only the
// scanner does -- and this module's OnEyeDraw is never consulted outside
// that window.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// fix.fss_panel_distance, resolved against fix.panel_distance. Install and
// reload paths; live.
void fssPanelConfigure(Config& cfg);

// One bool for the draw path's early-out set.
bool fssPanelWantsDraws();

// Called for eye draws. Matches X n=6 i=1, then the bound vertex shader's
// content hash against the pair. Remembers which shader matched for
// begin; true means wrap the draw in fssPanelBegin/End.
bool fssPanelOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                       uint32_t instances);

// Swap in the replacement for the matched shader / restore the game's.
void fssPanelBegin(ID3D11DeviceContext* ctx);
void fssPanelEnd(ID3D11DeviceContext* ctx);

void fssPanelShutdown();

}  // namespace edvr
