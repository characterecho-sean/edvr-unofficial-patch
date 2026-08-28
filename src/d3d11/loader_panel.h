// The loading dialog's full-view backdrop, collapsed onto the dialog's box.
//
// THE DEFECT. While Elite's loader shows a progress dialog, a dark bordered
// panel is drawn at the FULL size of the interface surface. On a monitor that
// is an ordinary modal scrim and nobody minds. In a headset it spans the
// field of view, and with the menu backdrop fix showing good art underneath
// (backdrop_fix.h) what it covers is worth seeing. The field's ideal, in
// their words: collapse it to "the exact size and shape" of the modal.
//
// THE MODEL, measured across four flights on 2026-08-28 and read out of
// vs 666EF0C4C616F67E's own disassembly (docs/shaders/ui-widget-vs.asm).
// Elite's widget system draws every solid panel as the SAME 30-index
// bordered quad set spanning one normalized space (about +/-32765), and
// places it with a PER-ELEMENT 4x4 MATRIX read from a structured buffer at
// VS t0 (stride 160; offsets 64/80 of the element feed the pixel shader
// its styling). Each vertex selects its element with a BYTE packed in its
// COLOUR attributes -- offset 12 or 16 of the 24-byte vertex, chosen by
// flag bits 0x4000/0x8000 in VS cb2[2].x. That is why three rasterizer
// architectures died measuring nothing: vertices, viewports and scissors
// are identical on every panel -- full-space, full-surface, off -- and the
// census never looked at VS resources at all. The roles are told apart by
// colour (RGBA8 at vertex offset 8: the scrim is black at alpha 0x66, the
// box black at 0xFF, the letterbox white) and CONFIRMED by each element's
// own matrix footprint -- the scrim's maps to the full view, the box's to
// the modal's rect, the exact rect the field wants the scrim collapsed to.
//
// THE MECHANISM.
//
//   Measure: when the loader's frame composition -- the sequence of draw
//   shapes into the interface surface -- holds identical for two
//   consecutive frames, capture one frame: every textureless quad-batch
//   draw's indices (the shared vertex buffer once), plus the widget table
//   (VS t0) and the flag constants (VS b2), all GPU-timeline copies.
//
//   Classify: panels by colour, then VERIFY through the same matrices the
//   shader will use -- the translucent panel's element must map to the
//   full view, the opaque one's to a boxed rect. A classification that
//   fails its own footprint refuses, with each panel's element and mapped
//   px rect in the log.
//
//   Substitute: the scrim's 30 vertices verbatim, with ONLY the two
//   element-index bytes rewritten to the box's. The game's own shader then
//   reads the BOX'S matrix from the live table, this frame and every
//   frame: the modal can move, resize or animate and the scrim follows,
//   because nothing about its placement is ever stored here. Positions are
//   trusted only while the frame matches the measured sequence draw by
//   draw; any divergence runs stock until the next stable window.
//
// EVERY failure -- no stable window, no table bound, mixed tables, no
// translucent full-view element, no boxed opaque element, an index beyond
// the captured window -- draws stock and says why in the log, once per
// measured shape. Stock is the game's own full-view scrim: safe, just big.
//
// KNOWN RISK, accepted until a flight rules on it: if the game reallocates
// element slots frame to frame within one stable dialog (the sun-glare
// hunt met exactly that in the 3D HUD), a captured index would point at a
// different element later. A static dialog most likely keeps its slots;
// wrongness would be immediately visible and immediately reported by the
// engage line's element numbers.
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
// STATUS: rebuilt 2026-08-28 on the element-index model, read from the
// shader itself rather than guessed; awaiting the confirming flight.
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
