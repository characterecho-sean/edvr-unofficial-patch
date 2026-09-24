// The pool probe -- recognises the game's per-instance pose records (a
// 336-byte record in a structured buffer the scene's vertex shaders index
// at t33: bone base at byte 0, scale at 4, the orientation as four unorm16
// at 8, the position at 16 -- the head the FSS shader replacement already
// transcribes, fss_panel_vs.h; docs/per-object-motion.md question 5) and,
// once an eye run's ledger is armed (objectProbeArmLedger below), copies
// that pool every frame for tools/eye_run_ledger.py: which records were
// drawn, how far each turned between consecutive crops, how the bones
// moved, and how the draws' own matrices turned. See THE EYE RUN'S LEDGER
// below for what it keeps, and objectProbeOnEyeDraw for the recognition.
//
// RETIRED 2026-09-23. This file used to also ESTIMATE each rigid mover's
// own motion for the temporal pass (tier 2 of docs/per-object-motion.md):
// a worker thread diffed two copies of the pool eight frames apart,
// clustered the rigid motions it found, tracked the stepped parts and
// moving ships, and published each one's motion for the pass's body path
// (fix.temporal_aa_objects). Nothing here estimates a per-object
// transform any more; it is superseded by the engine-record path,
// docs/kinematic-motion-injection-2026-09-19.md. What survives above is
// the pool recognition and the ledger capture it feeds.
//
// A mapped WRITE_DISCARD pointer is write-combined memory and reading
// megabytes of it on the CPU costs milliseconds, which is why the ledger's
// per-frame copy is the GPU's and the readback is late, never waited on.
// The recognition runs with fix.temporal_aa or advanced.object_probe (one
// shader-resource read a second); the copies only while an eye run is armed.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// advanced.object_probe, live.
void objectProbeConfigure(Config& cfg);

// For the draw chain's early-return list: true while the probe is on.
//
// Inline: asked per eye draw, and the build has no /GL to fold a cross-TU
// getter for two bool loads.
namespace detail {
extern bool g_objectProbeOn;
extern bool g_objectProbeLedgerOn;
}  // namespace detail
inline bool objectProbeWantsDraws() {
    return detail::g_objectProbeOn || detail::g_objectProbeLedgerOn;
}

// One eye draw, after the eye gate: an instanced draw may be asked for its
// t33 binding, a few times a frame until the pool is known and then once a
// second. One bool when the probe is off. count is the draw's vertex or
// index count and startInstance its StartInstanceLocation -- the ledger's
// row (objectProbeArmLedger below); nothing else reads them.
void objectProbeOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances,
                          uint32_t startInstance,uint32_t start=0,int32_t base=0);

// Visible draws routed before the normal eye-draw hook (particle billboards).
// The caller establishes that the target is an eye. Record the original
// shader in an active ledger, without probing its unrelated instance pool.
inline bool objectProbeLedgerActive() { return detail::g_objectProbeLedgerOn; }
void objectProbeSourceDrawEnd(ID3D11DeviceContext* ctx);
// Around only the native draw, after Begin substitutions and before private
// depth/motion reissues. Inactive outside an explicitly armed eye capture.
void objectProbePanelDrawBegin(ID3D11DeviceContext* ctx);
void objectProbePanelDrawEnd(ID3D11DeviceContext* ctx);
void objectProbeNoteSourceDraw(ID3D11DeviceContext* ctx,char kind,uint32_t count,uint32_t instances,
                              uint32_t startInstance,uint32_t start,int32_t base);
void objectProbeNoteGuiSourceDraw(ID3D11DeviceContext* ctx,char kind,uint32_t count,uint32_t instances,
                                 uint32_t startInstance,uint32_t start,int32_t base);
void objectProbeNoteEarlyDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                             uint32_t instances, uint32_t startInstance,uint32_t start=0,int32_t base=0);

// THE EYE RUN'S LEDGER (2026-09-10). The run's raw crops say how much each
// part of a station turned from one frame to the next; the pool says how
// much its RECORDS turned. At ten kilometres the two disagreed -- the drawn
// ring at a third of its records' turn, the hub's face at nearly all of it
// (the flight of 04:16) -- and nothing in hand said which draws paint the
// ring at that range, from which records, through which bones. So the key
// that takes the run also arms this: for the same frames, every frame's
// pool copy is kept; the scene's instance stream (the per-instance record
// indices every pool draw reads, 8 bytes each) and the first megabyte of
// every bone palette the pool draws bind at VS t38 (up to four) are copied
// and kept; every eye draw is noted with its vertex and pixel shader hashes,
// its counts, its start instance, whether t33 was the pool, and the render
// target it landed in (the census's intern id: the two eye targets, any
// offscreen target, or none for a depth-only pass); and the big instanced
// draws that
// read no pool (the first run's ledger, 05:19: the whole station's records
// were drawn and turned as one, so the slow ring is drawn by something
// else -- a ring of segments placed by a world matrix, say) have their
// per-draw constant buffer, t0 and first two vertex buffers copied at each
// frame's first draw -- all written beside the crops after the run, for
// tools/eye_run_ledger.py: which records were drawn, how far each turned
// between consecutive crops, how the bones moved, and how each of those
// draws' matrices turned. Nothing of it runs unarmed. stamp is the run's
// HHMMSS, shared with the crops' names.
// drawstate_<stamp>.bin also keeps each watched celestial/holo draw's VS
// b0/b1/b2 and PS b2, plus the first t2 image of each holo surface. These
// draws often have one instance and were absent from the old aux capture.
// tools/eye_draw_snapshot.py reads them. The watched VS bytecode is kept
// at creation and written with the run; no broad shader-dump switch is needed.
void objectProbeArmLedger(const wchar_t* stamp);
// The pass took the run's crop k this frame: the ledger notes the frame.
void objectProbeLedgerMark(int k);

// The frame edge, with the owner context: while an eye run's ledger is
// armed, the run's per-frame pool copy and its late readback.
void objectProbeFrameBoundary(ID3D11DeviceContext* ctx);

void objectProbeShutdown();

}  // namespace edvr
