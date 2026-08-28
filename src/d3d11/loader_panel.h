// The loading dialog's backing, sized to the dialog.
//
// THE DEFECT. While Elite's loader shows a progress dialog, the panel behind
// it -- a dark fill inside four border strips -- is drawn at the FULL size of
// the interface surface. On a monitor that is a modal scrim and nobody
// minds. In a headset it spans the field of view, and with the menu backdrop
// fix showing good art underneath (backdrop_fix.h) what it covers is worth
// seeing. The dialog itself is a fraction of that area.
//
// WHY A MEASURED FIX AND NOT A FACTOR. The first version of this took a
// hand-tuned ratio, fix.loading_panel_scale, and the field's verdict on it
// was the right one: a magic number is wrong on a rig whose interface surface
// is a different size (that size moves with render scale), wrong again when
// the loader shows its second, taller dialog, and wrong for good when
// Frontier changes either. The panel should be sized to the thing it backs,
// and the thing it backs is measurable.
//
// WHAT IS MEASURED. Every draw into the interface surface in one frame, read
// out of the shared vertex buffer they all use:
//
//   * the PANEL is the one whose quads span essentially the whole surface --
//     the census measured it as five quads, a fill inset by fifteen units
//     inside strips fifteen and twenty-seven thick, at +/-32765;
//   * the DIALOG is the union of every other draw's quads, with anything that
//     also spans the whole surface excluded, because a second full-surface
//     rectangle is another backdrop and not content.
//
// The panel's vertices are then copied verbatim and their positions mapped
// linearly from its own bounds onto the dialog's, plus a margin. Only the
// float2 at offset 0 is touched, so colour and everything else in the
// 24-byte vertex travels through untouched and no encoding has to be
// decoded -- the one insight that keeps this small.
//
// WHAT IT DOES NOT DO. It does not remove the panel: the dialog needs a
// backing and this is that backing, in the right place. It does not touch the
// wash layered over it, which is fix.loading_dim's job (docs/loading-scrim.md)
// and a different mechanism entirely.
//
// STATUS: FLOWN AND WRONG, kept because the measurement is the finding.
//
// The first field run (2026-08-28) returned, against a panel spanning
// 65529x65529:
//
//   the dialog it backs measures 913x568
//                                11221x568
//                                    0x0
//
// 913 is 1.4% of the panel, and the panel duly collapsed to a sliver. The
// recurring 568 is what names the mistake: that is the height of one LINE OF
// TEXT. What this measures as "the dialog" is a text run, not a dialog box.
//
// And there is no dialog box to find, because THE PANEL IS THE DIALOG'S BOX.
// Everything else in that buffer is its contents. "Size the panel to the
// dialog it backs" is circular: the panel is the thing that gives the dialog
// an extent. The premise is wrong, not the arithmetic.
//
// The draw counts per measurement -- 1, 3, 6, 11, 9, 12 -- name a second
// fault: collection runs for one frame and catches a different subset each
// time, so even content-derived bounds would not be stable as written.
//
// WHAT A FOURTH ATTEMPT WOULD HAVE TO DO. Size the panel to its CONTENT plus
// padding, not to a "dialog" that does not exist separately; and collect
// reliably, which means either accumulating across frames until the set of
// draw shapes repeats, or hooking the buffer's own Map to see every element
// written in one pass. It would also want to establish whether all these
// draws share ONE coordinate space -- this code assumes they do, and nothing
// yet proves it. See docs/loading-panel-handoff.md.
//
// SCOPE. The intro only. The main menu is a rendered hangar with a dark layer
// of its own -- a different one, which survived emptying this very buffer --
// and nothing here should reach it. kSceneEyeDraws is that boundary already
// measured for this module: menu-shaped frames peak around twenty draws, a
// rendered scene clears a hundred.
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
// fix is on and the frame is loader-shaped. Feeds the measurement and answers
// whether THIS draw is the panel and a substitute is ready for it.
bool loaderPanelOnDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                       uint32_t instances, uint32_t startIndex, int baseVertex,
                       uint32_t targetW, uint32_t targetH);

// Swallow the panel's draw and re-issue it from the resized geometry. False
// means nothing was drawn and the caller must draw stock -- which is the
// state for the few frames a measurement takes.
typedef void(__stdcall* PfnDrawIndexedInstanced)(ID3D11DeviceContext*,
                                                 unsigned int, unsigned int,
                                                 unsigned int, int,
                                                 unsigned int);
bool loaderPanelSubstitute(ID3D11DeviceContext* ctx, PfnDrawIndexedInstanced draw,
                           uint32_t instances, uint32_t startInstance);

// Once per frame: retire a measurement whose copies have had time to execute,
// decode it, and build the geometry. Cheap when nothing is pending.
void loaderPanelTick(ID3D11DeviceContext* ctx);

void loaderPanelShutdown();

}  // namespace edvr
