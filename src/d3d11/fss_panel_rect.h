// The scanner screen's rectangle in the eye, derived per engage.
//
// THE MEASUREMENT (2026-08-27, probe flights 1-5, docs in the ledger):
// the FSS's visible screen is a camera-centred quad pair -- the display
// at +-131.176 x +-71.874 model units, 156.114 ahead, and its frame at
// +-164.700 x +-92.655, 180 ahead: 16:9 to four digits, identity
// orientation, record position zero, the world-rebase origin centimetres
// from the camera. The only session-varying input is the fused per-eye
// view-projection at the composite's cb0 rows 4..7 -- and while the
// theater is engaged the game renders from a FROZEN pose, so those rows
// are constant for the whole engagement.
//
// THE DERIVER: at the first recognised chrome composite of an engage
// (the theater's mode latch open), stage-copy cb0, read it back a few
// draws later, project the frame's four corners on the CPU, and publish
// the rect through the bridge for the theater's crop. One derivation per
// engage; the latch closing resets it. Every failure publishes nothing
// and the theater falls back to its centred-band crop.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

// Called at a recognised chrome composite (the tracker's matched branch)
// while fix.fss_theater is armed. Runs the stage/readback/derive state
// machine; free once the engage's rect is published.
void fssPanelRectOnComposite(ID3D11DeviceContext* ctx);

void fssPanelRectShutdown();

}  // namespace edvr
