// The loading dialog's full-view backdrop, collapsed onto the dialog's box.
//
// THE DEFECT. While Elite's loader shows a progress dialog, a dark bordered
// panel is drawn at the FULL size of the interface surface. On a monitor that
// is an ordinary modal scrim and nobody minds. In a headset it spans the
// field of view, and with the menu backdrop fix showing good art underneath
// (backdrop_fix.h) what it covers is worth seeing. The field's ideal, in
// their words: collapse it to "the exact size and shape" of the modal.
//
// THE MODEL, measured across three flights on 2026-08-28. Elite's widget
// system draws every solid panel as the SAME 30-index bordered quad set in
// one normalized space (about +/-32765 across) and SIZES it with the
// VIEWPORT: the frame holds several such panels whose vertices are
// indistinguishable while the screen shows a full-view scrim, a modal-sized
// box and a letterboxed frame. The roles are told apart by the two things
// that do differ:
//
//   * COLOUR -- an RGBA8 at vertex byte offset 8. The scrim is black at
//     alpha 0x66 (the 40% wash the field called ugly); the box is black at
//     alpha 0xFF; the letterbox is white.
//   * VIEWPORT -- the scrim's spans the surface; the box's is the modal's
//     own screen rect, which is exactly the rect the field wants the scrim
//     collapsed to.
//
// Three earlier architectures died before this was measured. A hand-tuned
// ratio was rightly rejected in the field. A measured fit unioned "everything
// else" and caught one line of text mid fade-in (913x568 against 65529x65529
// -- the panel collapsed to a sliver), and its failure was misread as "the
// panel IS the box" because every probe of the day matched draws by
// signature and so hit scrim and box together. A seeded growth then found
// that no solid's VERTICES are modal-sized at all -- vertex bounds are
// per-viewport and comparing them across draws was never meaningful.
//
// THE MECHANISM.
//
//   Measure: when the loader's frame composition -- the sequence of draw
//   shapes into the interface surface -- holds identical for two consecutive
//   frames, capture one frame: every textureless quad-batch draw's indices
//   (the shared vertex buffer once) for the colours, and each draw's
//   viewport and scissor for the rects. Stability gating exists because the
//   dialog fades in over many frames; the old code measured whichever
//   transitional frame it woke in.
//
//   Classify by position: the SCRIM is a 30-index panel, dark and
//   translucent, with a full-surface viewport; the BOX is dark and opaque
//   with a boxed viewport (or an enabled boxed scissor). Positions are
//   trusted only while the frame matches the measured sequence draw by
//   draw; any divergence runs stock until the next stable window.
//
//   Substitute: the scrim's own draw call is re-issued THROUGH THE BOX'S
//   VIEWPORT, read fresh at the box's draw every frame. The widget system
//   itself then sizes the scrim into the modal's rect -- no geometry is
//   built, no vertex is touched, and the box, its border and its text are
//   the game's own draws, forwarded bit-identically.
//
// EVERY failure -- no stable window, no translucent full-view panel, no
// boxed opaque panel, a stale box rect -- draws stock and says why in the
// log, once per measured shape, naming each solid it saw with its colour
// and rects. Stock is the game's own full-view scrim: safe, just big.
//
// WHAT IT DOES NOT DO. It does not remove the scrim -- inside the box's
// rect it still composites exactly as the game intended, under the box. It
// does not touch the frosted wash layered over the whole view, which is
// fix.loading_dim's job (docs/loading-scrim.md) and a different mechanism
// entirely.
//
// SCOPE. The intro only. The main menu is a rendered hangar with a dark
// layer of its own -- a different one, which survived emptying this very
// buffer -- and nothing here should reach it. kSceneEyeDraws is that
// boundary already measured for this module: menu-shaped frames peak around
// twenty draws, a rendered scene clears a hundred.
//
// STATUS: rebuilt 2026-08-28 on the viewport model, third architecture of
// the day; awaiting the flight that confirms the box's viewport is boxed.
// docs/loading-panel-handoff.md carries the full evidence trail.
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
