// The depth-of-field blur, averaged over the jitter's cycle (flat profile, the copy route).
//
// Below native the resolve runs after the tone, so the game's blur is made from the single jittered frame. A very bright glint jitter hits on some phases and misses on
// others is spread by the blur into a large shape that changes from frame to frame, and the temporal resolve keeps that as real change: bright out-of-focus highlights
// flicker with AA on and hold still with it off. (docs\design-flat-temporal-aa-2026-09-23.md, "2026-11-05, flight 05:11".)
//
// So the composite reads the mean of the last frames' blur in place of this frame's, for its one draw, and the game's view goes back after it. In-focus pixels come from
// the scene image as before. It runs where a jittered frame reaches a composite on the copy route with the camera still; every other case leaves the game's blur bound and
// restarts the average (flat_dof_blur_math.h says which). There is no setting: nothing here applies with AA off, and scenes with no composite pay nothing.
#pragma once

#include <d3d11.h>

#include <cstdint>

namespace edvr {

// Right before the game's issue of a draw flatDofBlurComposite names. True: `original` holds the game's view, for End.
bool flatDofBlurBegin(ID3D11DeviceContext* ctx, uint64_t frame, bool jittered, bool hdrRouteTreated, uint32_t phaseCount, uint64_t ps, ID3D11ShaderResourceView** original);
void flatDofBlurEnd(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* original);
// The copy route's resolve, once a frame: rows 4 and 5 of its camera, for the next frame's composite.
void flatDofBlurNoteCamera(uint64_t frame, const float (*camera)[4]);
// Once a frame at Present: lets the images go when no composite has run for a while.
void flatDofBlurFrame(uint64_t frame);
// The 5 s window's line. Silent where no composite ran: a window with composites and no line is a build or a path that never reached this.
void flatDofBlurLogWindow();
void flatDofBlurRelease();
void flatDofBlurNoteDevice(ID3D11Device* device);

}  // namespace edvr
