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

// Reads advanced.transition_flash_resubmit and experimental.submit_snapshot.
// Call where the other openvr-side config reads happen -- install AND the
// reload poll: the snapshot mode is an in-headset A/B experiment, and a
// toggle that costs a relaunch per look is not an experiment anyone runs
// twice.
void resubmitShadowConfigure();

// Does the forward path want every frame submitted as a snapshot?
//
// WHY (2026-08-25, the FSS ring split, round three). The game side measured
// symmetric at every level: both eyes composite the same body texture, no
// writes land between their reads, and the composite's constants differ
// only as per-eye camera placement (the DCW dumps). What no capture can see
// from the CPU is WHEN the compositor samples each eye's texture -- Elite
// reuses one texture per eye with no fence, the second eye's GPU work is
// issued last, and under build load the compositor can catch eye A finished
// and eye B still drawing: one eye persistently a frame ahead, monocularly
// real, exactly the report. Submitting a per-frame COPY of each eye latches
// both eyes' delivered content at the same point in the frame: whatever the
// compositor's timing, it reads completed copies enqueued back to back --
// split staleness becomes symmetric staleness, which is the pair latch's
// own CONSISTENT-LATE-BEATS-SPLIT rule applied to pixels.
bool resubmitShadowSnapshotWanted();

// After a Submit that was actually FORWARDED (TextureType_DirectX only):
// refresh this eye's copy from the submitted texture. The copy is queued
// after the game's rendering of that frame, so its contents are the
// completed frame. eye is 0 (left) or 1 (right). Returns true when the copy
// landed THIS call -- the snapshot path submits the copy only on a fresh
// refresh, because handing the compositor a stale copy for one eye is the
// exact asymmetry the mode exists to remove.
bool resubmitShadowNoteForwarded(uint32_t eye, void* d3d11Texture);

// The current copy, for snapshot submission. No counters, no shape probe --
// the caller uses it immediately after a true NoteForwarded, which just
// rebuilt the copy at the live shape. Null when there is none, and the
// caller submits the live texture exactly as before: the mode must never be
// worse than its absence.
void* resubmitShadowCurrent(uint32_t eye);

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
