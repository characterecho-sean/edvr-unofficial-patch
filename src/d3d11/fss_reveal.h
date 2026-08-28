// The FSS reveal, evaluated at one moment for both eyes.
//
// THE MECHANISM (docs/shaders/fss-composite-ps.asm, closed 2026-08-25 after ten
// instrumented rounds): the body composite's pixel shader is an ordered
// dissolve -- a reveal PROGRESS value in the 5,328-byte scene block at PS
// b1 (cb1[119]) is compared against each cell's own luminance-plus-noise
// threshold; unrevealed cells show the previous layer, in-transition
// cells get the outlined border, and a stage index selects the layer
// pair. Nothing in that math is per-eye: the quad UVs, the textures and
// the thresholds are identical in both eyes. The one thing that CAN
// differ is the b1 CONTENTS at each eye's draw -- the block is rewritten
// between the two composites, and a progress value evaluated per WRITE
// steps between them, so every cell whose threshold falls inside the step
// is revealed in one eye and not the other: black squares in exactly one
// eye, only while the animation runs, converging when it saturates, mono
// in cinema mode. The channel was invisible to every earlier capture
// because the composite has no PS b0 and nothing ever recorded PS b1.
//
// THE FIX is the exposure fix's idea at the constant-buffer level: the
// first eye's composite draw has its PS b1 contents shadowed (from the
// Map/Unmap tee vscreen already owns), and the second eye's composite is
// drawn with those exact bytes in an EDVR-owned buffer, restored after
// the draw. Both eyes then evaluate the dissolve at the same moment.
// Whichever moment wins, the SPLIT is gone -- both eyes show the
// animation the way the flat screen does.
//
// THE SECOND CHANNEL (2026-08-26, the Toolkit-off steady flight): steady
// engaged with receipts and the squares survived a clean pipeline -- so
// the constants are unified and the eyes still differ, and the only
// remaining stepper is the CONTENT of the shared input textures between
// the two draws. The first eye composites the earlier fill state (black
// tiles), the second the later one. A retired redraw mode re-issued the
// first eye's whole draw after the second's, and its flight bought the
// frame-order fact that killed it: the game draws the first eye's UI
// BETWEEN the two body composites, so a late repaint buries that UI --
// there is no moment both after the content fills and before it.
// lockstep inverts the direction instead: occurrence 1's INPUTS are
// frozen -- the four content textures CopyResource'd at its draw, the
// scene block snapshotted -- and occurrence 2 is drawn reading those
// byte-identical copies. The two panels then CANNOT differ, by
// construction. lockstep is HALF the shipped black-squares fix, paired
// with fss_eye_heal: the squares' arrival-window frames are drawn BELOW
// this composite by the game's per-eye temporal reconstruction (the heal
// covers those), and lockstep holds the eyes identical through the
// composite-drawn resolve that follows. steady stays as an instrument.
//
// Configured by fix.fss_eye_sync -- ONE key for the whole fix
// (src/common/eye_sync.h); this module serves the composite half (the
// lockstep and steady mechanisms). Free when neither is armed.
// Recognition rides the body-frame gate and the composite's vertex
// hash, the fss_probe pattern exactly.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

void fssRevealConfigure(Config& cfg);

// One bool for the draw path's early-out set and the body-frame gate.
bool fssRevealWantsDraws();

// The Map/Unmap and UpdateSubresource tees, called from vscreen's hooks
// while steady: the scene block's writes keep the shadow current, so the
// snapshot at eye A's draw is exactly the bytes eye A read. Both paths,
// because the first flight proved assumptions about which one a buffer
// uses cost whole sessions.
void fssRevealNoteMap(void* resource, void* data);
void fssRevealNoteUnmap(void* resource);
void fssRevealNoteUpdate(void* resource, const void* data);

// Called for eye draws behind the body-frame gate: matches the composite
// (N n=6 i=1 + vh 953C8123AD8DC13B). True wraps the draw in Begin/End.
bool fssRevealOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                        uint32_t instances);

// Occurrence 1: the PS b1 buffer is learned and its shadow snapshotted;
// lockstep also freezes the draw's four content textures. Occurrence 2:
// steady substitutes the snapshot and lockstep binds the frozen copies,
// both restored after the draw.
void fssRevealBegin(ID3D11DeviceContext* ctx);
void fssRevealEnd(ID3D11DeviceContext* ctx);

// Per-frame occurrence reset, from vScreenFrameBoundary.
void fssRevealFrameBoundary();

void fssRevealShutdown();

}  // namespace edvr
