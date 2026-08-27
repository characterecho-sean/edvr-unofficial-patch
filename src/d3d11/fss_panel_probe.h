// One capture of everything the FSS screen quad's transform reads.
//
// THE QUESTION (2026-08-27, the field's ask): the scanner's screen is a
// quad composited into the stereo scene -- roughly 16:9, the "virtual
// panel" -- and the theater wants its exact rectangle in the eye so the
// cinema screen can crop to precisely it. The transform is documented
// (docs/fss-panel-vs.asm; the replacement pair in fss_panel_vs.h): world
// position from a t33 instance record (uniform scale, unorm16x4
// quaternion, position at byte 16), clip from the fused view-projection
// at cb0[4..7]. If the record's camera-relative placement is FIXED --
// one probe answers that -- the rect becomes a runtime formula from the
// projection tangents EDVR already has, and no game buffer is ever read
// again.
//
// THE INSTRUMENT, panel_quad's staging dance exactly: at one recognised
// chrome-composite draw (vh + panel-scale slot 1, and ONLY while the
// theater's mode latch is open -- the same pipeline draws the loading
// screen, round 34's lesson), box-copy the record head, CB0, and the
// vertex bytes to staging; map them a few frames later when the copy has
// certainly executed; log floats and hex; stand down for the session.
//
// advanced.fss_panel_probe = 0|1. One capture per session. Free when off.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

void fssPanelProbeConfigure(Config& cfg);

// True while armed and no capture has been taken this session.
bool fssPanelProbeWants();

// The composite's DrawIndexedInstanced arguments, stashed by the thunk --
// the record index the draw actually uses starts at startInstance.
void fssPanelProbeDrawArgs(uint32_t startIndex, int32_t baseVertex,
                           uint32_t startInstance);

// The latest stashed startInstance -- the rect deriver reads the
// instance-stream window of the very draw it captures with.
uint32_t fssPanelProbeStartInstance();

// Called at a recognised chrome composite (inside the tracker's matched
// branch). Copies on the first call, reads back and logs a few frames
// later, then stands down.
void fssPanelProbeOnComposite(ID3D11DeviceContext* ctx);

void fssPanelProbeShutdown();

}  // namespace edvr
