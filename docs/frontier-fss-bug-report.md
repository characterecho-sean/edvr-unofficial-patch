# FSS (VR): hard-black unresolved tiles shown to one eye only at every zoom arrival

**Type:** Bug report — VR rendering
**Area:** Full System Scanner, zoomed body view
**Game version:** 4.4.0.3, build r330683 (reproduced across 2026 builds; the behaviour is long-standing)
**Severity:** Cosmetic per event, but it fires on every FSS zoom and produces binocular rivalry — one of the most frequently player-reported VR issues with the scanner.

---

## Summary

When the FSS zooms onto a body in VR, the zoomed image arrives with a set of
hard-black (0.000-luma) 16×16-pixel tiles that are shown to **one eye only**
(the primary eye — the same view the flat-screen mirror shows; the left eye on
both rigs tested). The other eye receives the same content with those tiles
already lit. For roughly the first ten frames after the zoom transit lands
(~100 ms), the two eyes therefore disagree: one eye sees black squares
scattered over the body and its ring, the other sees the finished image. The
tiles then resolve in unison and the eyes agree again.

Because each eye is shown *different* content in the same screen region, the
result is binocular rivalry rather than a subtle artifact — the squares
shimmer and fight, and the effect reads far worse in a headset than its
duration suggests. It repeats at every zoom arrival, i.e. dozens of times in a
normal system scan.

This is distinct from the FSS's intended tile-dissolve animation. That
animation (cells popping in with bracket-outlined borders over ~3 s) is drawn
**symmetrically to both eyes** and looks fine. The bug is specifically the
arrival window in which not-yet-resolved tiles are rendered hard black in one
eye while the other eye already has them.

## Steps to reproduce

1. Stock installation, no mods, VR enabled (HMD headphones mode).
2. Fly to any system with a ringed planet (rings make it most visible, but
   any bright body shows it).
3. Open the FSS, tune to the body, and zoom in to the fully-zoomed body view.
4. Watch the body/ring during the moment the zoom arrives, with either eye
   closed and then the other (or through the per-eye mirror).

**Expected:** both eyes receive the same resolve state; whatever dissolve
animation plays is identical in both eyes.

**Observed:** at arrival, the primary eye's submitted image contains hard-black
unresolved tiles that the other eye's image does not (measured 16 tiles vs 2
on the same frame). They persist ~10 frames, then resolve simultaneously.
Repeatable on every zoom; the affected eye never swaps sides.

Reproduced on two independent stacks, stock:

- Pimax headset, OpenComposite → vendor OpenXR runtime
- Meta Quest 3 via Steam Link, SteamVR

Same eye (left/primary) on both, so runtime/compositor causes are excluded.
Host: Windows 11 Pro, GeForce RTX 5090.

The flat-screen mirror (which follows the primary view) also shows the tiles
briefly; on a monitor they read as part of the dissolve art, which is likely
why this survives flat-screen QA. Only stereo exposes the asymmetry.

## Renderer-level characterization

We instrumented the D3D11 stream (draw/dispatch/copy census, per-eye
render-target dumps at multiple pipeline checkpoints, and per-eye captures of
the submitted textures) while reproducing the bug. Findings, in pipeline
order:

1. **The dissolve itself is symmetric.** The body-layer composite draw is
   byte-equivalent between the eyes: same quad, same textures, and forcing its
   scene-constant block identical for both eye draws changes nothing.
   Full-image tile diffs of both eyes' render targets taken pre-ring,
   post-ring, post-composite and at frame end during the build show the same
   blinking-tile population in both eyes at every checkpoint, at the same
   world positions (view-shifted). The animation is intended art and is
   delivered to both eyes.

2. **The asymmetry is confined to the zoom-arrival window.** A per-frame
   tile-luminance series over both *submitted* eye textures across the arrival
   shows the primary eye carrying hard-black (0.000 luma) tiles on bright
   content that the secondary eye does not — 16 vs 2 on the worst frame —
   for ~10 frames, resolving in unison afterwards.

3. **The zoomed body is produced by a per-eye, GPU-driven, temporally
   amortized tile renderer.** The relevant passes are `DispatchIndirect`
   compute (group counts from GPU-written argument buffers, per-eye tile-list
   buffers, one thread group per 16×16 tile — exactly the square granularity),
   writing eye-sized HDR targets (R11G11B10_FLOAT plus an R16 auxiliary), with
   a per-frame history-copy storm maintaining per-eye accumulation state.
   During the arrival frames in question, the screen content comes from this
   compute path — no draw-call composite runs in those frames.

4. **The mechanism consistent with all measurements:** at each zoom arrival
   the tile renderer restarts and warms up per eye under a shared tile budget,
   and the primary eye's history/warm-up lags the secondary's. Tiles the
   budget has not yet reached are rendered hard black rather than holding
   previous content — so the lagging eye shows black squares over regions the
   leading eye has already filled. Once both eyes' accumulation is warm the
   images converge and stay converged.

Internal shader/bytecode identifiers, full census logs, and the per-eye
capture series are available on request.

## Suggested fix directions

Any one of these would eliminate the visible defect:

- **Warm both eyes symmetrically at zoom arrival** — reset/seed both eyes'
  tile-renderer state on the same frame and schedule the tile budget so the
  two eyes' resolve fronts advance together.
- **Hold, don't black:** initialize unresolved tiles from the previous zoom
  level's image (or the last resolved state) instead of hard black, so a
  budget lag between eyes is a subtle sharpness difference rather than black
  squares.
- **Gate display on both-eyes-ready:** don't present a tile in either eye
  until both eyes have resolved it (the dissolve animation already provides
  the cover for the wait).

## Impact note

The FSS is one of the most-used exploration screens, and VR players hit this
on every body they zoom. The per-eye disagreement (rather than the squares
themselves) is what players report: it defeats stereo fusion in the center of
the view exactly where they are looking. A fix here would close out a large
share of the long-running VR complaints about the scanner.

---

*Report prepared from renderer-level instrumentation done for the EDVR
community patch project. All measurements above were confirmed on stock,
unmodified installations. Happy to provide captures, logs, or to run
diagnostic builds.*
