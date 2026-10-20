#pragma once
// TEMPORARY hangar bloom measurement. Remove with the arc, including hotkey.bloom_probe.
//
// The image path is unchanged while the state stays jitter=on aa=on (the state at startup).
// F6 cycles jitter on/off x the AA pass on/off. It does not write the ini.
//
// What the log shows when a probe never ran (do not read a missing line as zero):
//   no "bloom-probe:" line at all
//       bloomProbeTick never ran (present did not reach the flat runtime).
//   "hotkey: ... bound nothing" and no "bloom-probe: state" line
//       the key is off, so the four states cannot change. The image stays jitter=on aa=on.
//   "bloom-probe: tick ... missed=p0" and no "bloom-probe: p0 frame=" line
//       H was not submitted at the HDR trigger. On the copy route this is expected when the
//       trigger does not see H. A submitted point that has not mapped yet is absent from missed=
//       and still has no stats line until a later tick; "readback failed" means the sample was
//       taken and the numbers are not zeros.
//   missed=p1    H was not submitted after the write point (AA off and the copy route submit p1
//                as the same H as p0, so a miss there means that submit did not run).
//   missed=p2    bloom downsample VS DFED8E1C9E191BEC / PS 143AAE0597E2F7BF was not seen.
//   missed=p3    tonemap VS F9CFC798F21E9AEA / PS E8948389387DA083 was not seen, and the copy
//                source was not submitted in its place.
//   missed=p3f   the copy route did not produce an upscaler output (AA off, or the HDR route).
//   missed=strip compute shader F1FB2EEFB662F3AA did not run, or its u1 was not a 6-wide texture.
//   "not taken" / "gpu resources failed" / "staging failed" / "target srv failed" / "slots full"
//       the point was reached and was not measured. Those lines are not zeros.
// A stats line always carries samples= and frame=. missed=none means every point was submitted.
#include <cstdint>
#include <d3d11.h>

namespace edvr {
enum class BloomProbePoint : uint8_t { HBefore = 0, HAfter, Bloom, Tone, Final, Count };

bool bloomProbeJitter() noexcept;
bool bloomProbeAaPass() noexcept;
// True until this window has submitted the point. Callers use it to avoid work on the other frames.
bool bloomProbeDue(BloomProbePoint point) noexcept;
bool bloomProbeStripDue() noexcept;
// One-shot after the AA pass has been off: the next resolve drops history, so the accumulation
// delay is visible when the pass comes back. A frame that skips the pass does not consume it.
bool bloomProbeTakeHistoryReset() noexcept;
uint64_t bloomProbeFrame() noexcept;

// The frame the draws are in. Clears the remembered route when the frame changes.
void bloomProbeNoteFrame(uint64_t frame) noexcept;

// Present, before the next frame's phase is chosen. Polls the hotkey and lagged readbacks.
void bloomProbeTick(ID3D11DeviceContext* context, uint64_t frame);

void bloomProbeNoteRoute(bool hdrRoute, uint64_t frame);

// Once a second per state. No CPU stall: the numbers are logged when a later tick maps them.
void bloomProbeSample(ID3D11DeviceContext* context, ID3D11ShaderResourceView* srv, BloomProbePoint point, uint64_t frame);
void bloomProbeSampleTarget(ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv, BloomProbePoint point, uint64_t frame);

// After F1FB2EEFB662F3AA. `uav` is the shader's u1. Ignored unless it is a 6-wide texture.
void bloomProbeNoteStrip(ID3D11DeviceContext* context, ID3D11UnorderedAccessView* uav, uint64_t frame);

bool bloomProbeIsBloomDraw(uint64_t vs, uint64_t ps) noexcept;
bool bloomProbeIsToneDraw(uint64_t vs, uint64_t ps) noexcept;
bool bloomProbeIsStripShader(uint64_t hash) noexcept;
}
