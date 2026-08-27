// The FSS reveal, evaluated at one moment for both eyes.
//
// THE MECHANISM (docs/fss-composite-ps.asm, closed 2026-08-25 after ten
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
// tiles), the second the later one. redraw is the fix for that channel:
// the first eye's composite draws stock and its draw is CAPTURED -- eye
// target, viewport, geometry buffers and VS constants copied at that
// instant -- then RE-ISSUED immediately after the second eye's
// composite, when the shared textures and scene block hold the late
// state both eyes should show. Its own transforms, the current content:
// both eyes paste the same fill state, whichever eye draws first.
//
// fix.fss_reveal_sync = stock | steady | redraw. Stock by default until
// the field look; free when stock. Recognition rides the body-frame gate
// and the composite's vertex hash, the fss_probe pattern exactly.
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

// Occurrence 1: steady learns the PS b1 buffer and snapshots its shadow;
// redraw captures the draw's target, viewport, geometry and VS constants.
// Occurrence 2: steady substitutes the snapshot, restored after the
// draw; redraw re-issues occurrence 1's draw after this one completes.
void fssRevealBegin(ID3D11DeviceContext* ctx);
void fssRevealEnd(ID3D11DeviceContext* ctx);

// The composite's DrawInstanced arguments and the REAL draw pointer,
// stashed by the DrawInstanced thunk when the verdict is kFssReveal --
// the begin/end wrap never sees them, and the redraw's re-issue must
// call the real function (calling the context re-enters our own hook).
typedef void(__stdcall* FssRevealRealDraw)(ID3D11DeviceContext*,
                                           unsigned int, unsigned int,
                                           unsigned int, unsigned int);
void fssRevealDrawArgs(uint32_t startVertex, uint32_t startInstance,
                       FssRevealRealDraw realDraw);

// Per-frame occurrence reset, from vScreenFrameBoundary.
void fssRevealFrameBoundary();

void fssRevealShutdown();

}  // namespace edvr
