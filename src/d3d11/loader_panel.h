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
// WHERE THE BOX ACTUALLY IS, settled by seven flights and one screenshot.
// The dialog's solid black backing is NOT in the interface surface at all:
// every quad of every interface draw was captured, matrix-verified and
// colour-estimated across three hunt generations, and nothing there
// renders a dark boxed rect. The eye-level census shows where it lives --
// each eye's frame ends with a textured quad, a constant-buffered quad
// with depth (stride-8 vertices: a positioned rect), and then the
// 5760-index composite that lifts the interface in. The backing is an
// EYE-LEVEL layer under that composite, kin to the menu's dark layer that
// famously survived emptying the interface buffer.
//
// THE MECHANISM, which that discovery makes almost embarrassingly small.
//
//   Measure IMMEDIATELY: the first frame that shows a 30-index panel is
//   captured -- every textureless quad-batch draw's indices (the shared
//   vertex buffer once), plus the widget table (VS t0) and the flag
//   constants (VS b2), all GPU-timeline copies, polled with DO_NOT_WAIT
//   so the verdict lands a frame or two later. No stability wait: the
//   classification is frame-local, so a mid-fade frame is a valid sample,
//   and the field's report that the scrim showed briefly at first was the
//   old stability gate's cost, not a necessity. The scrim appears first
//   in a text-over-scrim phase BEFORE the dialogs, so the withhold is
//   normally live before any modal exists.
//
//   Classify: the scrim is the standalone panel that is dark, translucent,
//   and whose element maps its fill to the full view -- verified through
//   the same matrix the shader will use, so a misclassification cannot
//   survive its own footprint.
//
//   Withhold BY ORDINAL: the verified scrim is the k-th panel of its
//   surface in the frame (k = 0 in every measurement to date), an
//   identity that is frame-local and immune to the text churn that broke
//   positional matching. It is swallowed every frame -- fade-in, percent
//   ticks and the dialog switch included -- for as long as panels keep
//   arriving (one hiccup frame forgiven; the census shows menu frames
//   carry no panels, so the chain cannot outlive the loader), and the
//   classification is re-verified about every two seconds. Inside the
//   box, 40% black over the backing's opaque black was invisible --
//   withholding is pixel-identical to a perfect collapse there. Outside
//   the box, the tint was the defect, and now does not exist.
//
// EVERY failure -- no stable window, no table bound, mixed tables, no
// dark translucent full-view element -- draws stock and says why in the
// log, once per measured shape. Stock is the game's own full-view scrim:
// safe, just big.
//
// WHAT IT DOES NOT DO. It does not touch the dialog's backing, border or
// text -- the backing is an eye-level draw this module never sees, and the
// content draws are forwarded bit-identically. It does not touch the
// frosted wash layered over the whole view, which is fix.loading_dim's
// job (docs/loading-scrim.md) and a different mechanism entirely.
//
// SCOPE. The intro only. The main menu is a rendered hangar with a dark
// layer of its own -- a different one, which survived emptying this very
// buffer -- and nothing here should reach it. kSceneEyeDraws is that
// boundary already measured for this module: menu-shaped frames peak around
// twenty draws, a rendered scene clears a hundred.
//
// STATUS: final form 2026-08-28 -- the withhold -- after seven flights and
// a screenshot placed the backing at eye level; awaiting the confirming
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

// Once per frame: close the frame's composition record, arm or poll a
// collection, maintain the withhold chain. sceneFrame is the caller's
// scene boundary (eye draws cleared kSceneEyeDraws last frame): the first
// such frame means the intro is over, and this module retires for the
// session -- the documented scope, enforced. Cheap when nothing is
// pending.
void loaderPanelTick(ID3D11DeviceContext* ctx, bool sceneFrame);

void loaderPanelShutdown();

}  // namespace edvr
