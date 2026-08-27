// The scanner screen's rectangle in the eye, derived per engage.
//
// THE MEASUREMENTS (probe flights 1-5 and the crop flights, 2026-08-27):
// the scanner draws SEVERAL quads through one pipeline on one atlas --
// the display, its outer frame, UI boxes, and scenery pieces around the
// player (the neon frame). The records churn per frame, their indices
// are fetched from the slot-0 instance stream, and only same-frame data
// coheres. The screen family is camera-centred (record position ~zero);
// the scenery is not (sub-metre positions, rotated). The eye pair draws
// each quad back-to-back, and which eye is first is NOT assumed: the
// left eye is identified by its projection (the screen's centre lands
// nasal of image centre only in the left eye).
//
// THE DERIVER, once per engage: across one frame it captures every
// matched draw's own instance entry and vertex window (same-frame, at
// that draw), cb0 for BOTH eyes of the first pair, the cb1 rebase row,
// and the whole record pool. Three frames later it decodes each quad,
// classifies screen family versus scenery, projects the family's
// perspective-normalised union through the LEFT eye's rows, fit-shrinks,
// and publishes the corners for the renderer's homography -- and hands
// vscreen a per-ordinal skip mask so the scenery draws are dropped while
// the cinema screen is up. Every failure publishes nothing: the theater
// keeps its centred band and no draw is skipped.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

// The composite's DrawIndexedInstanced arguments, stashed by the thunk
// before recognition runs -- the capture windows use the very draw's own
// values. (Inherited from the retired survey probe.)
void fssPanelRectDrawArgs(uint32_t startIndex, int32_t baseVertex,
                          uint32_t startInstance);
uint32_t fssPanelRectStartInstance();
int32_t fssPanelRectBaseVertex();

// Called at EVERY recognised chrome composite while the theater or the
// heal is armed and the mode latch is open. ordinal counts matched draws
// within the frame (0-based); startInstance/baseVertex are the draw's
// own.
void fssPanelRectOnComposite(ID3D11DeviceContext* ctx, uint32_t ordinal,
                             uint32_t startInstance, int32_t baseVertex);

// The per-ordinal draw policy the derivation produced: bit i set means
// matched draw ordinal i is SCENERY and should be skipped while the
// screen is up. 0 until a derivation lands (skip nothing).
uint32_t fssPanelRectSkipMask();

void fssPanelRectShutdown();

}  // namespace edvr
