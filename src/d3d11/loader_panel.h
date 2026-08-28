// The loading dialog's full-view backdrop, collapsed onto the dialog's box.
//
// THE DEFECT. While Elite's loader shows a progress dialog, a dark bordered
// panel is drawn at the FULL size of the interface surface. On a monitor that
// is an ordinary modal scrim and nobody minds. In a headset it spans the
// field of view, and with the menu backdrop fix showing good art underneath
// (backdrop_fix.h) what it covers is worth seeing. The field's ideal, in
// their words: collapse it to "the exact size and shape" of the modal.
//
// THE MODEL, corrected on 2026-08-28. The frame holds TWO kinds of dark
// rectangle, not one:
//
//   * the BACKDROP: a five-quad bordered panel -- fill plus four edge strips,
//     30 indices -- spanning the whole coordinate space (measured 65529 wide
//     at +/-32765). This is the thing to shrink.
//   * the BOX: the modal the player actually sees, drawn ON TOP of the
//     backdrop. Field-observed on both intro dialogs -- a solid black
//     backing behind "PREPARING SHADERS", an orange-bordered frame around
//     the taller second dialog. Either its own 30-index panel or solid quads
//     inside a larger batch; this module handles both.
//
// Two earlier architectures died for want of that distinction. A hand-tuned
// ratio (fix.loading_panel_scale) was rightly rejected in the field: a magic
// number is wrong on the next rig and the next dialog. A measured fit then
// sized the panel to "the union of everything else", which on a fade-in frame
// is a TEXT RUN: it measured 913x568 -- one line of text -- against a
// 65529x65529 panel and collapsed it to a sliver. The failure was read as
// "there is no box, the panel IS the box", and that conclusion was wrong: the
// probes that seemed to prove it (census_skip_offscreen, census_skip_quad)
// match every draw sharing a signature, so the backdrop and the box -- both
// X:30 -- always vanished together, and the one-shot quad probe only ever
// sampled the first of them. What LOOKED like one batched call was two.
//
// THE MECHANISM.
//
//   Measure: when the loader's frame composition -- the sequence of draw
//   shapes into the interface surface -- holds identical for two consecutive
//   frames, capture one frame: every textureless quad-batch draw's indices,
//   plus the shared vertex buffer, GPU-copied to staging and read back once
//   the copies have certainly executed. Stability gating exists because the
//   dialog fades in over many frames; the old code measured whichever
//   transitional frame it woke in, which is why its collections returned 1,
//   3, 6, 11, 9, 12 draws on six consecutive attempts.
//
//   Classify: the backdrop is any 30-index draw with a quad spanning most of
//   the widest and tallest extent in the capture. The TARGET is the union of
//   every other solid's quads -- the box's own panel dominates that union
//   when it exists, and the bare backing rectangle is found inside its batch
//   when it does not. Full-span sheets and edge-riding strips stay out of the
//   union; textured draws (the text) were never in it, which is what makes
//   the old text-run circularity impossible by construction.
//
//   Substitute: the backdrop draw alone is swallowed and re-issued from its
//   own vertices with positions remapped linearly onto the target bounds --
//   colour and the rest of the 24-byte vertex travel through untouched, so
//   nothing about the encoding has to be decoded. The box, its border and
//   every glyph on it are the game's own draws, forwarded bit-identically.
//   Substitution is by POSITION in the frame's draw sequence, valid only
//   while the frame matches the measured sequence up to that position; the
//   moment composition diverges -- animation, dialog change -- later draws
//   run stock and the next stable window re-measures. A dialog switch shows
//   the previous target for the few frames a fresh measurement takes.
//
// EVERY failure -- no stable window, no backdrop found, no solids beside the
// backdrop (the scrim shows alone before the dialog arrives), a sliver-sized
// or full-surface union -- draws stock and says why in the log, once per
// measured shape. Stock is the game's own full-view panel: safe, just big.
//
// WHAT IT DOES NOT DO. It does not remove the backdrop -- the box's designed
// look keeps a dark ground behind it, in the right place. It does not touch
// the frosted wash layered over the whole view, which is fix.loading_dim's
// job (docs/loading-scrim.md) and a different mechanism entirely.
//
// SCOPE. The intro only. The main menu is a rendered hangar with a dark
// layer of its own -- a different one, which survived emptying this very
// buffer -- and nothing here should reach it. kSceneEyeDraws is that
// boundary already measured for this module: menu-shaped frames peak around
// twenty draws, a rendered scene clears a hundred.
//
// STATUS: rebuilt 2026-08-28 on the corrected model; awaiting a field
// flight. docs/loading-panel-handoff.md carries the full evidence trail.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Reads fix.loading_panel (stock | fit). Install and reload; live.
void loaderPanelConfigure(Config& cfg);

// False in stock mode, which keeps the per-draw path free when off.
bool loaderPanelWants();

// Called for every draw into an interface-sized offscreen surface while the
// fix is on and the frame is loader-shaped. Records the frame's composition,
// feeds a pending measurement, and answers whether THIS draw is a backdrop
// with a substitute ready. textured says whether the draw reads a PS slot-0
// texture -- text does, the solids this module cares about do not.
bool loaderPanelOnDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                       uint32_t instances, uint32_t startIndex, int baseVertex,
                       uint32_t targetW, uint32_t targetH, bool textured);

// Swallow the backdrop's draw and re-issue it from the collapsed geometry.
// False means nothing was drawn and the caller must draw stock -- the state
// for the few frames a measurement takes.
typedef void(__stdcall* PfnDrawIndexedInstanced)(ID3D11DeviceContext*,
                                                 unsigned int, unsigned int,
                                                 unsigned int, int,
                                                 unsigned int);
bool loaderPanelSubstitute(ID3D11DeviceContext* ctx, PfnDrawIndexedInstanced draw,
                           uint32_t instances, uint32_t startInstance);

// Once per frame: close the frame's composition record, arm or validate a
// collection, retire a settled measurement into built geometry. Cheap when
// nothing is pending.
void loaderPanelTick(ID3D11DeviceContext* ctx);

void loaderPanelShutdown();

}  // namespace edvr
