// The Full System Scanner's reveal fix (flight d7769063, docs/fss-scanner.md): at the first reveal composite of a frame, the frame's first downsample of the ring layer is run again from the
// finished layer, so the left eye's coarse input matches the right eye's and neither eye shows the black squares. The decisions are fss_reveal_core.h, the mechanism fss_reveal_replay.h; this
// is the hook glue: which draws are recognised, the log, and the scanner-session tallies.
//
// VR profile only, no key. Free when the scanner is not on show: the draw hook asks one bool.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

namespace detail {
extern bool g_fssRevealWatch;   // VR profile and the scanner is on show: the DrawInstanced hook asks fssRevealOnDrawSlow
}  // namespace detail

inline bool fssRevealWatching() { return detail::g_fssRevealWatch; }

// The game's own un-hooked entry points, so the replay is invisible to the hooks that track state (any may be null: the context's member is then called).
struct FssRealCalls {
    void* vsSetCb = nullptr; void* psSetCb = nullptr; void* psSetSrv = nullptr; void* vsSetSrv = nullptr; void* vsSetShader = nullptr; void* psSetShader = nullptr;
    void* omSetRts = nullptr; void* omSetBlend = nullptr; void* rsSetViewports = nullptr; void* drawInstanced = nullptr;
};

// Called for every DrawInstanced on the owner context while the watch is on. Records a downsample's state; at the first reveal composite of the frame, replays it.
void fssRevealOnDrawSlow(ID3D11DeviceContext* ctx, const FssRealCalls& calls, uint32_t perInstance, uint32_t instances, uint32_t startVertex, uint32_t startInstance);

// Once a frame, from the frame boundary. `onShow`: the scanner screen is up (the FSS mode latch is open, or its chrome matched within five frames). `flat`: the flat profile, where it stays off.
// Tallies the frame that ended, turns the watch on and off, and writes the session summary when the scanner has been off show for 30 frames.
void fssRevealFrameBoundary(bool onShow, bool flat, uint32_t frameNo);

// Said once per process: whether the fix is armed in this profile, so its absence from a log means the code never ran.
void fssRevealConfigure(bool flat);

// Writes the summary of a session still open, and releases the replay's resources.
void fssRevealShutdown();

}  // namespace edvr
