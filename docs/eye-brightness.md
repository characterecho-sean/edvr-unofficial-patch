# One eye darker than the other near bright lights

*Frontier issue [47312](https://issues.frontierstore.net/issue-detail/47312)*

## Status

*Updated 2026-10-14. Branch `claude/retire-share-exposure`.*

**State.** The engine path is FLOWN CLEAN and the setting is RETIRED. In VR the
second eye's two exposure dispatches (the bounds pass and the exposure shader) are
not sent to the GPU, always; eye 0's result is copied into eye 1's five UAVs at the
point where the second exposure dispatch would have run. The trace, the code and the
tests are in "The engine path (2026-10-13)" at the end of this file. The Scroll Lock
toggle (`hotkey.toggle_exposure`) went 2026-10-13.

**Retired 2026-10-14: `fix.share_exposure`** (`share_exposure = 1` in `[fix]`, "Both
eyes share one brightness"; 1 on, 0 off, read at startup; F8 menu row, restart).
Flight evidence, build 57f5393e (`claude/flight-2026-10-14`), 19 min: the second
eye's chain skipped at 2.00 a frame, 175,018 dispatches cumulative, 0 fallbacks, no
stood-down, and both eyes matched near a star. The key, its template block, its menu
row and its off-is-stock path are gone, the way the terrain cull cover's were. The
fail-safe stays, because it is behaviour: a chain whose shape does not match what
the engine path expects runs whole, then the copy. A stale `share_exposure =` line
in someone's ini is harmless (config audit, once). Flat never read it: its key
allow-list never listed it, so `getBool` answered false there and the flat dispatch
hook returns before the exposure code runs. Flat is unchanged.

**Open (each has a log signature in the flight check at the end).**
- Readers of eye 1's `u0`, `u2`-`u4` and strip texels 1-5 other than the copy are
  unproven. The tonemap's vertex shader reads strip texel 0 (disassembly). The
  copy keeps every reader where it is; that is why it is a copy and not a binding.
- The exposure shader must bind exactly UAV slots 0-4 when it runs. If the game
  leaves a stale UAV in slots 5-7 the engine never engages and says so after 300
  frames (`the exposure shader does not bind exactly UAV slots 0-4`).
- The 30 s line's saving is the 2026-10-13 pass-profile figure times the count.

**Ruled out.**
- ruled out: re-pointing eye 1's consumers at eye 0's strip (a binding
  substitution), because the one reader the disassembly proves is a vertex-stage
  slot the census cannot see, other readers are unproven, and it needs a hook on
  the shader-resource bind calls on a CPU-bound path to save five tiny copies.
- ruled out: skipping eye 1's three 252x252 draws (VS `DEF19B03`), because they
  cost 0.0095 ms of the 0.16, and a 320x180 exposure output is read later in the
  frame (flat census, unproven for VR).
- ruled out: eye 1's adaptation history needing its own coverage. The only history
  in the chain is strip texel 0, read and written by the exposure shader, and the
  old copy already overwrote it with eye 0's every frame.
- ruled out: a live toggle, because bringing the second chain back mid-session
  means re-seeding history; the toggle and then the setting were dropped instead.

**Next flight.** None needed. `edvr_log.py --grep "share exposure"` shows the
`engaged` line and the 30 s lines (`fallbacks` stays 0), or one line naming a reason.

A write-up of what causes the per-eye brightness difference in VR, how it was
measured, and what a fix inside the game would be. Written for whoever might fix
it there.

**Short version: auto-exposure is computed independently for each eye.** Near a
bright light one eye can see the light and the other cannot, so the two eyes
choose different exposures and disagree about how bright the world is.

---

## The symptom

Approach a star, or stand near a station floodlight, and one eye stops down while
the other does not. The effect is strongest when the light is at the edge of
view — where it falls inside one eye's frustum and outside the other's — and it
is most obvious on the background rather than on the light itself.

It is uncomfortable in a way that is hard to place until you know what it is, and
impossible to unsee afterwards. Binocular rivalry from a sustained brightness
mismatch is not something the visual system settles into.

## What is actually happening

The auto-exposure luminance range is computed **per eye**, into **separate
persistent resources**. They are disjoint: never shared, never copied between.

The exposure chain keeps four per-eye resources, stable across frames:

| Slot | Contents |
|---|---|
| u0 | histogram bounds, 8 bytes |
| **u1** | **exposure parameters, 6×1** |
| u2 | histogram, 384×1 |
| u3 | histogram, 384×1 |

The compute shader that produces the bounds performs a min/max reduction over the
eye's own downsampled HDR view, seeded with the standard log-luminance limits —
`13.287712` is log2(10000) and `−9.828280` is log2(0.0011) — and stores the
`(min, max)` pair. A second shader consumes that and builds the tonemap curve,
using `0.301030`, which is log10(2), the stops conversion.

So the eye that can see the star reduces a view with a much higher maximum
luminance. Its range shifts, and its **whole view** stops down.

## How it was measured, and why it is exposure rather than glare

The whole view stopping down is the part that identifies the mechanism, and it is
measurable. Splitting each eye's image into tiles and comparing them on a held
pose:

| Region | Difference between eyes |
|---|---|
| Background — every tile except the brightest 25% | **+0.957 stops** |
| The bright tiles | **−1.118 stops**, the *opposite* direction |

The eye that can see the star is brighter *at the star* and darker *everywhere
else*. That is what stopping down looks like, and it is not what any localised
effect looks like: glare, bloom, or an occlusion-query artifact leaves the
background untouched by definition. A control on a scene with no bright source
shows no bias in either direction.

Independently, with the workaround disabled, the same measurement returned
**+0.975 stops** against the +0.957 above — the same number from a separate
session, which is what makes it a property of the renderer rather than of one
capture.

## What a fix inside the game would look like

Compute the exposure reduction **once for the stereo pair** rather than once per
eye.

The two obvious forms:

- **Share.** Compute it for one eye and use the result for both.
- **Union.** Take the min of the mins and the max of the maxes, then derive one
  exposure from that. Because the payload is a `(min, max)` pair rather than a
  scalar, this is the more principled variant — it is the exposure appropriate to
  everything either eye can see.

Either removes the divergence. Neither costs a pass: the second eye's reduction
becomes unnecessary rather than additional.

**One thing worth knowing before choosing.** Sharing is directional, and the
direction is not arbitrary. Copying from the *first* eye rendered to the second
is correct. Copying the other way produces a numerically closer match between the
eyes — a residual of −0.135 stops against −0.32 to −0.58 — and is nevertheless
wrong: it darkens the whole scene, dropping peak tile luminance from 0.58 to
0.35, because the second dispatch belongs to the eye that can actually see the
star. The better-looking number comes from throwing away the brighter eye's
information. A union avoids the question entirely, which is the main argument for
it.

## What should *not* be removed

Once both eyes hold byte-identical exposure parameters, a residual difference of
about −0.3 to −0.6 stops remains. That residual is **correct**. It is the bloom
around a star that one eye can see and the other cannot, and it is real stereo
content — the same thing your eyes would do looking at a real light source past
the edge of an obstruction.

The goal is not two identical images. It is two images that agree about how
bright the world is.

---

## What EDVR does about it

It copies the per-eye exposure resources from the first eye to the second, each
frame, at the point where the second eye's exposure shader runs. Both eyes then
tonemap from one set of parameters. Until 2026-10-13 the second eye's shader ran
and the copy overwrote what it wrote; now that shader, and the bounds pass before
it, are not sent to the GPU for the second eye at all (see "The engine path"
below).

That is one `CopyResource` per resource per frame and no memory patching — the
game's code is untouched.

Measured with a live toggle (since removed), on held poses (within 0.024 m and 3.5°):

| | Background difference between eyes |
|---|---|
| Fix off | **+0.975 stops** |
| Fix on | **−0.170** and **−0.610 stops** |

The measurement above was taken with a Scroll Lock toggle that existed until
2026-10-13. There is no switch now (the setting was retired 2026-10-14); stock is
what an older build does.

The pass is identified by **what it does** — the shader that writes the exposure
parameters, found by its read/write magnitudes and confirmed by its disassembly —
rather than by a shader hash, so a game update does not break it. If it cannot
find it, it does nothing and says so in the log.

### A note on method, since it cost the most time

Two earlier candidate shaders were rejected, both `1×1×1` dispatches with a UAV
that looked like terminal reductions and were not: one was an indirect buffer
clear, the other indirect dispatch argument setup. **Dispatch shape is not
evidence.** What identified the real pass was reads-to-writes magnitude plus
reading the disassembly.

The first attempt at the fix also did nothing, and the reason is worth recording:
it shared only the 8-byte `(min, max)` buffer, which turns out to be histogram
*bounds* used to scale a 128-bin axis — not the exposure itself. The copy fired
correctly, between two genuinely distinct resources, and changed nothing. Only
capturing all four UAV slots showed where the exposure parameters actually live.

---

## The engine path (2026-10-13)

*Request: fix the exposure sharing in the engine path, for performance. Eye 1's
exposure chain does not run; eye 1 consumes eye 0's result. The key, its values and
its menu row stay. Research: `reviews/vr-view-caches-2026-10-13.md`, candidate 1.*

### The chain, per eye (disassembly of the game's shaders, not inference)

Both eyes' chains run back to back, each followed by that eye's tonemap draw
(census `edvr_gfx_20261012_083703.log`, q 1793-1815: `B8E727E9`, `F1FB2EEF`,
tonemap eye 0, `B8E727E9`, `F1FB2EEF`, tonemap eye 1).

| Step | Reads | Writes | History |
|---|---|---|---|
| three small draws, 252x252 (VS `DEF19B03`) | the eye's HDR | the small textures | none |
| CS `B8E727E9` (24x24 threads) | the two small textures | u0, an 8-byte (min, max) of log luminance, rewritten whole; u1, a 252x252 luminance map | none |
| CS `F1FB2EEF` (1024 threads, five UAVs) | t0 = that map (same resource as B8's u1), t1, u0, **u1 texel 0** | u1 (the 6x1 strip), u2 (histogram, 384x1 uint), u3, u4 (384x1 float) | **u1 texel 0 only**: `ld_uav_typed u1` at line 317 of the listing, stored back at line 331 |
| tonemap draw, VS `2D78DC3F` | VS t0 at (0,0) = strip texel 0, passed on as the exposure scalar | the eye's image | none |

The PS of the tonemap (`99C21CEB`) reads only the 3D grading texture and the HDR.
Sources of the listings: `tools/dxbc_disasm.py` on `cs_F1FB2EEF...`, `cs_B8E727E9...`,
`vs_2D78DC3F...` from the shader dump.

### What the old copy covered and missed

`shareExposure` (main, `exposure_fix.cpp:323-346`) copied u0-u3 from eye 0 to eye 1
after the second `F1FB2EEF` had run. Missed: u4 (never copied, `slot < 4`). Eye 1's
whole chain therefore computed values that the copy then overwrote, apart from u4,
which only the histogram stage writes and the census shows no draw or dispatch
reading (resource-level check below).

Resource-level readers, by `res=` identity in the census id tables (frame at
08:42:10.010): eye 1's u0, u2, u3, u4 and map resources appear only in their own
`B8E727E9` / `F1FB2EEF` dispatch lines and in EDVR's copy lines (`DCC`). Eye 1's
strip resource appears in the same places; the tonemap VS reads it through a
vertex-stage slot the census does not list. Eye 0's strip is read by ~400 scene draws in
the frame's body (previous frame's value), which the copy never touched.

### What is skipped, what is copied

- Skipped, when engaged: the second `B8E727E9` and the second `F1FB2EEF`
  (identified by position in the frame: the second occurrence of the learned
  predecessor and of the exposure shader, with the learned order B8 F1 B8 F1).
  `src/d3d11/exposure_chain.h` is the decision; `exposure_fix.cpp:818` asks it, and
  `:840` forwards the dispatch unless it said skip. The census still records what
  the game submitted, before the decision.
- Not skipped: eye 0's chain, and the three small draws of either eye.
- Copied (where the second exposure dispatch was): eye 0's u0-u4 over eye 1's, five
  `CopyResource` calls, 8 B + 24 B + 3 x 1.5 KB. u4 is new. This is the old copy
  with one more slot, not a second mechanism: it also runs when the engine stands
  down and the second chain runs stock.
- Why a copy: see Status ("ruled out"). A substitution cannot be proven to leave
  every reader whole; the copy does that by construction.

### Adaptation history, the damper, the fifth UAV

- History: strip texel 0 of each eye. Eye 0's is read and written by eye 0's
  exposure shader as before. Eye 1's was read by eye 1's shader and then replaced by
  the copy every frame, so what eye 1's shader computed never fed back. Skipping it
  changes nothing; eye 1's texel 0 is eye 0's after every confirmed frame, in both
  versions. On frames where the fix is not confirmed (menus) both chains run stock
  and the copy waits, exactly as before, and the first confirmed frame overwrites
  eye 1 with eye 0, exactly as before.
- Damper (`experimental.exposure_damping`): `exposureDamp` reads the UAV view bound
  at the second exposure dispatch (taken from the bind calls, so a skipped dispatch
  still has it) and the strip after the copy. Unchanged, and the same bytes.
- Fifth UAV: now copied. It is read nowhere the census can see; the copy is
  coherence, not a measured need.

### Fail-safe

The engine engages only after ten whole eye-chains (five frames) in which the
dispatch before the exposure shader shared its u0 and bound u0 and u1, the exposure
shader bound exactly UAV slots 0-4, the two eyes had different u0 views, the copy
covered all five slots on a stock frame, and the detection was confirmed. Nothing is
hard-coded: the predecessor hash is learned. A frame that disagrees (view recreated,
a predecessor on another u0, a different order) runs that chain whole and counts a
fallback. A skipped predecessor not followed by the second exposure dispatch, a UAV
set that changes after the predecessor was skipped, or a copy that comes up short
after a skip, stands the engine down for the session and says so once. Running the
second chain is always safe: the copy lands after it, as in stock.

### Toggle and reload

Until 2026-10-14 `share_exposure` was read once, at install, and the F8 row was
flagged restart. It is retired: `enabled` is now `!runtimeFlatProfile()`, set once at
install. A stale `toggle_exposure =` or `share_exposure =` line in a user's ini is
harmless: the build's config audit names it once in the log ("lines name settings
this build does not read"), nothing reads it, and the installer's merge carries a
changed value to the end of its section with a note.

### Tests (all in the build gate)

`tools/exposure_chain_test` (`:rig_exposure_chain_test` in `build.bat`): the decision
(learning, the skip, every decline, fallback and latch, the predecessor renamed by a
game update), three pipelines of stand-in chain shaders on a WARP device with the
debug layer (stock, the old copy, the engine path) that must agree bit for bit on
eye 0 and eye 1 every frame through engaging, a lost confirmation and a stand-down,
the device's own compute-invocation count dropping by exactly the skipped
dispatches, and source pins on the hook glue and on the removal of the toggle. The
hotkey and config rigs were updated for the removed key.

### Flight check

Same exposure behaviour near a star with both eyes matching (the −0.17 / −0.61 stop
residuals above, not a new difference). Log: `share exposure: the second eye's
exposure chain is NOT sent to the GPU` once, then every 30 s `share exposure: eye-1
chain skipped N dispatches (saved ~X ms by the census), fallbacks F` with F at or near
0. No flash when entering or leaving a menu. The saving shows as about 0.15 ms less
GPU time a frame.
