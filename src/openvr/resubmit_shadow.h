// GENERATED from src/openvr/resubmit_shadow.h in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 e0590a4fcbe49788]
// The last forwarded frame, kept so a withhold can hand SteamVR a copy of it
// instead of a missed deadline.
//
// The judder was never the withholding -- it is the deadline. A declined
// Submit leaves the compositor waiting ~80 ms for a frame that never comes,
// paid identically for real flashes and false positives (6ag.2 measured it at
// 82 ms). EDVR holds a valid texture at every Submit; keeping a per-eye copy
// of the last FORWARDED frame turns a withhold into "submit the copy instead
// of nothing": an on-time, valid submit of one-frame-old content at the
// current pose -- reprojection's output without reprojection's stall.
//
// CopyResource only, no shaders -- the exposure fix's mechanism class. The
// thread-identity gate for calling the immediate context from the Submit
// callsite PASSED on 2026-08-15: hookedSubmit and hookedPresent both measured
// on thread 3108, so Submit runs on the render thread the game already uses
// for that context. Single-threaded use is therefore assumed here and stated:
// every function below is called from the Submit thread only.
//
// The fix must never be worse than its current self: any doubt -- no copy
// yet, texture shape changed, a faulting handle, the setting off -- answers
// nullptr, and the caller withholds classically, exactly as before this
// module existed.
#pragma once

#include <cstdint>

namespace edvr {

// Reads advanced.transition_flash_resubmit. Call where the other openvr-side
// config reads happen (install), before the first Submit.
void resubmitShadowConfigure();

// After a Submit that was actually FORWARDED (TextureType_DirectX only):
// refresh this eye's copy from the submitted texture. The copy is queued
// after the game's rendering of that frame, so its contents are the
// completed frame. eye is 0 (left) or 1 (right).
void resubmitShadowNoteForwarded(uint32_t eye, void* d3d11Texture);

// On a withheld frame: the texture to submit in place of the live one, or
// nullptr when classic withholding must happen this frame. NEVER copies --
// a withheld frame's content must not reach the shadow, so consecutive
// withholds repeat the same last-good frame, degrading exactly as classic
// withholding does but smoothly.
void* resubmitShadowForWithhold(uint32_t eye, void* liveD3d11Texture);

// For the totals lines: substitutions made, and withholds that fell back to
// classic because no acceptable copy existed at that moment.
uint32_t resubmitShadowResubmits();
uint32_t resubmitShadowFallbacks();

void resubmitShadowShutdown();

}  // namespace edvr
