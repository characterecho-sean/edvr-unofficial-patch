#pragma once

#include <d3d11.h>
#include <cstdint>

namespace edvr {
// Planet patch motion (docs/terrain-motion-dispatch-cost-2026-09-17.md, 2026-10-06 "the design").
//
// A body near range is drawn as cube-sphere surface patches (VS 72BDD292154158AD, the colour pass), and each patch's
// constants say where the body is that frame. In supercruise the camera rows carry the ship's turn but not its
// translation, so the world path gives a planet the camera's motion alone and DLSS blurs it on approach. This module
// reads the patch constants on the CPU -- no GPU copy, no reissue, no extra draw -- takes one rigid motion per body
// from two frames of them (src/common/celestial_math.h), and hands the temporal pass a small record buffer per eye:
// the body's view-space [R|t] and a coverage volume. A pixel on the world path with a depth inside a body's volume
// takes that body's motion in place of the camera's (decision path 12, `celestial`).
//
// HOW THE CONSTANTS ARE READ. The game writes the patch's VS b2 (and the camera rows in VS b0) with Map/Unmap
// (about 1100 Maps a frame; a DISCARD Map hands back a fresh allocation). vscreen.cpp's Map and Unmap hooks tee the
// write: at the Unmap, before the real one, the 384 bytes the patch needs are copied out of the mapped pointer into a
// shadow kept per watched buffer. UpdateSubresource is the same through celestialMotionConstantsWritten. A copy
// INTO a watched buffer, a command list, a re-created buffer at the same address: the shadow is invalid until the
// next write. At the draw (forwardWithVerdict) the shadow is read; a draw whose buffers are unwatched, never
// written since, or the wrong size is counted and left to the camera term -- never read from the GPU.
namespace detail {
extern bool g_celestialLive;
extern bool g_celestialAnyWatched;
}  // namespace detail

constexpr uint64_t kCelestialPatchVs = 0x72BDD292154158ADull;   // the colour pass; its PS is 76849D64AC657DB9

// The draw path's own first test, one load. Live = configured on (fix.temporal_aa in the VR build), not stood down, and the
// supercruise gate open (below).
inline bool celestialMotionLive() { return detail::g_celestialLive; }
// The tees' own first test: any VS constant buffer watched. Each tee below is a pointer compare against the watched
// table and nothing else, on a path the game takes about 1100 times a frame.
inline bool celestialMotionAnyWatched() { return detail::g_celestialAnyWatched; }

// Any thread: only a flag moves. State is rebuilt by the render thread when the module goes live.
void celestialMotionConfigure(bool enabled);

// THE SUPERCRUISE GATE (2026-10-08). The module runs only while the game's Status.json says supercruise (journal_watch.h,
// Flags bit 4). Outside it -- normal space, landed, on foot, docked -- the camera term already carries the ship's translation
// (v0.18.2 flew there without this path and drew no smear report), while a body's own rigid motion put on the world pixels of a
// docked eye was a station spin on every wall nearer than the planet (a Quest 3 log: docked, a planet 12,000 km off, the hangar
// a grainy mess). Not known counts as not supercruise. Gated off, celestialMotionLive() is false (the draw path's one load), the
// write tees are unwatched, celestialMotionRecords returns no SRV (probe.w bit 8192 clear, zero path-12 pixels), and what was
// captured is dropped, so the first frame after the gate opens again has no previous frame and takes that fallback.
// Render thread, once a frame, from the journal tick (device_hook.cpp's tickJournalGate): known = Status.json carried Flags.
// One log line per change of state. Not tracked while the module is configured off.
void celestialMotionNoteStatus(bool known, bool supercruise);

// The owner context's draw whose VS is kCelestialPatchVs, before the game's own issue: reads the shadowed constants
// into the eye's patch list. Render thread.
void celestialMotionNoteDraw(ID3D11DeviceContext* ctx);

// The write tees (vscreen.cpp). Mapped/Unmapped: a non-read Map's pointer, and the Unmap that ends it (called BEFORE
// the real Unmap). Written: UpdateSubresource's whole write, `box` the dst box or null.
void celestialMotionConstantsMapped(ID3D11Resource* resource, void* data);
void celestialMotionConstantsUnmapped(ID3D11Resource* resource);
void celestialMotionConstantsWritten(ID3D11Resource* resource, const void* data, const D3D11_BOX* box);
// A write whose bytes EDVR cannot see: a CopyResource/CopySubresourceRegion destination, a re-created buffer at a
// watched address, or nullptr for an executed command list (every watched buffer).
void celestialMotionConstantsUnknownWrite(ID3D11Resource* resource);

// One eye's records for the frame being treated: the temporal pass asks once per eye before it binds its inputs.
struct CelestialEyeRecords {
    ID3D11ShaderResourceView* srv = nullptr;   // the record buffer; null when no body has a record this frame
    uint32_t records = 0;                      // bodies with a record
    uint32_t bodies = 0, patches = 0, matched = 0;   // this eye's frame, for the motion trace
    double translation[3] = {};                // the nearest record's D translation, world-aligned metres
    double rotationDeg = 0.0;
    double distance = 0.0;                     // its nearest patch's distance
};
// Build (once per eye per frame) and upload the eye's records. tanNow is the pass's tangents (jitter excluded) and
// w x h the render size it dispatches over. True when out->srv is set.
bool celestialMotionRecords(ID3D11DeviceContext* ctx, int eye, const float tanNow[4], int w, int h, CelestialEyeRecords* out);

// Stats[39] of a diagnostic motion pass: the pixels that took decision path 12 (temporal_pass.cpp's poll).
void celestialMotionNotePixels(uint32_t pixels);

// Once per frame: the stamp the eyes' captures are paired by, and the 5 s census line.
void celestialMotionFrameBoundary();
// The render thread's shutdown: the GPU buffers and the watched table.
void celestialMotionShutdown();
}  // namespace edvr
