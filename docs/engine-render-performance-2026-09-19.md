# Engine render performance: a design space

## Status

- **State:** seeded 2026-09-19 (Sean: "secondary goal — improve engine
  render performance where possible"). Nothing built. This doc owns the
  performance arc; settlement-flicker-2026-09-17.md keeps the flicker
  diagnosis. The two share instruments and flights.
- **Open:** every lever below is a hypothesis until its named instrument
  produces a number. The first shared measurement is the job-bracket
  flight (probes installed 2026-09-19 evening, commit 116a2e0).
- **Ruled out (pointers, do not re-propose):** draw-call identity/motion
  estimation as a class — kinematic-motion-injection-2026-09-19.md.
  Compensation-style tweaks (sharpening over blur, threshold nudges) —
  AGENTS.md diagnosis discipline.
- **Next flight:** settlement flight on the 116a2e0 build; read with
  `python tools\edvr_log.py --target frontier --expect-build HEAD`.

## Frame budget philosophy

Root causes over compensation. Every lever must name (a) the measured
cost it attacks, (b) the instrument that proves the cost, (c) the
expected saving, (d) the observable regression risk. A lever without a
measurement does not get built — one flight per hypothesis is the most
expensive way to run this project, so instruments batch.

## What is measured so far (pointers, not restatements)

- ~23k D3D11 calls per frame at settlements, ~76% zero-sample draws
  (draws whose query never samples a pixel) — settlement-flicker arc,
  draw census + original-draw probe.
- KinematicRig eval FUN_14430EFE0 and its LOD-metric/traversal callers
  are the suspected settlement CPU tax; the sibling jobs (physics
  advance, render-data update, the 0x4320340 batch) are unmeasured.
- The 0x4320340 caller/job-dispatch path and the visibility masks around
  0x431B11D–0x431B221 (what decides a record dispatches at all) are
  unresolved — they gate any culling-side lever.

## Levers, cheapest measurement first

### L1. Know the split: CPU job brackets (first measurement 2026-09-19, eye run 205251)

First numbers: UpdateRenderDataJob ~7.0 ms summed per frame (138 calls
x 51 us), RenderDataBatch (0x4320340) ~2.4 ms, physics <= 0.05 ms.
**Do not read this as proved dominance** (Sean's review, settlement doc
21:23 entry): summed job durations can overlap across worker threads;
the detailed observer takes the probe mutex thousands of times per frame
inside the measured jobs; PrePhysicsAdvanceJob was never hooked. Next
measurement: brackets only (detailed observer disabled), job-3 included,
before any lever is sized on these numbers.

### L2. Draw suppression of proven-zero-sample draws

If the census shows the same draw families sample zero pixels for many
consecutive frames, suppress the original draw (not just skip our own
work). Engine-analogous to the visibility masks the game itself computes
at 0x431B11D–0x431B221 — resolve those first; if the game already has a
visibility bit we can tighten, prefer feeding it over second-guessing it
from the pipeline. Risk: suppressing a draw that becomes visible needs a
one-frame restore path, proven on the smoke/static-surface rigs.

### L3. Kinematic eval narrowing

**Blocked pending proven signals** (Sean's review, settlement doc 21:23
entry). The earlier formulation rested on two unproven foundations:
render-record+0x688 is the engine's own intra-frame evaluation sequencer
(its gates already consume it), and record+0x268 is a render-config hash
that never reads the transform — not a motion signal.

What the eval actually does per record per frame (FUN_14430EFE0,
decomp_430EFE0.txt): descriptor mask gate, node liveness bytes, two
flag-word skip gates, then view-dependent work — LOD distance test and a
plane-loop frustum test (FUN_1404f4e10). **LOD and frustum are
view-dependent: they must still run for stationary objects** (buildings
stand still while the head moves). The valid lever, if a clean L1
remeasure still shows eval work dominant, is narrower: avoid REBUILDING
unchanged object data while preserving every view-dependent decision.
What proves "unchanged" is unresolved — the transform/dirty-state
dependency chain is an open offline item.

### L4. Physics decimation for attached-static children

If physics jobs dominate: children rigidly attached to a static parent
do not need per-frame physics advance. Depends on the parent-physics
interface map (KinematicRig +0x1C0, slots +0x90/+0x170) proving which
classes distinguish static/kinematic/parent-relative — the other agent's
current reversing target. Do not build on planet attachment alone.

### L5. Render-thread / submission overlap

Only if L1 shows neither job set dominant: the 23k-call volume itself is
the tax. Options: deferred-context command lists for the swallow/redraw
paths we already own, or batching the swallow quads. Defer until the
openxr-submit-performance arc notes are re-read; there is history there.

## Cross-cutting constraints

- Every lever ships behind a config key naming the user-visible effect,
  off or auto by default per the existing convention.
- Every lever states its environment dependency at flight time: VR
  runtime, headset, per-eye render size, DLSS version, record-table
  sizes.
- Instruments are part of the deliverable. A lever that cannot report
  its own before/after cost in the log does not merge.

## 2026-09-20 (17:55) -- the L1 catch in code: timing is gated on the observer

Verified while wiring the job-attribution TLS mask (kinematic doc 17:50
entry, build installed to frontier): bracket() early-returns the
untimed forward when `!probe || !probe->active()`, so the QPC brackets
measure ONLY while the detailed observer is capturing. That is the L1
catch made concrete: probe on = the observer's per-record mutex/reads
inside every measured job; probe off = no numbers at all, even with the
tracker live (fix.engine_motion on). The 17:50 instrument does not
touch the timing path (its cost is one TLS read/OR/restore per job
call), so L1's remeasure is neither advanced nor worsened -- it is
blocked on exactly this gate.

The unblock is small and probe-side: hoist the QPC bracket off the
active() gate (time whenever the hooks own the job bodies), keeping
observe()/noteOwnership gated as today. Every tracker flight then
returns brackets-only job costs in the jobs[] JSON with no extra
flight, and a probe-armed window still gives the with-observer
comparison for the overlap correction. Semantics change to note:
jobs[] becomes per-session rather than per-capture-window. Job-3
(0x42DF530) still refuses the CodeHook (thunk-shaped leading
instruction) -- extend the hook for that prologue or accept the gap;
physics was <= 0.05 ms on eye run 205251, so job-3 is unlikely to
dominate, but the 21:23 remeasure note explicitly includes it.

If hoisted before the job-attribution flight, ONE flight batches both
measurements: the mover/static discriminator census AND the L1
brackets-only numbers.
