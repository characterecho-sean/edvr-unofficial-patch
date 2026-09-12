# Review: smoke-trail voids after the station fixes, 2026-09-10

**Historical review through `44edb30`.** The later `627d62f` build moves
smoke depth to a private target and reconstructs its depth, addressing the
live-DSV hazard described below. The [latest-run review](review-smoke-toggles-2026-09-10.md)
covers that build and the effects still visible with the switches off.

Reviewed `claude/fervent-galileo-a4ede2` through `44edb30` (clean worktree).
The latest Steam graphics log identifies the running code as
`v0.14.1-156-ga9c3a7a`; `44edb30` only changes documentation. This review
compares it with `3340c87`, the source examined in the earlier
[distance-and-motion review](review-distance-motion-2026-09-10.md).

The two station reproductions now pass. The smoke changes expose controls
but retain the previous behaviour at their defaults, which the installed
INI still uses. The captures clearly show the rectangular gaps. They
isolate the ribbon from the smoke volume in one run, but do not yet prove
whether the missing colour originates before or inside DLSS.

No rendering source, installed DLL, or installed setting was changed for
this review. Review programs and converted images are in the ignored
`build/review_motion` directory of the review checkout.

## Branch changes checked

- `944ba90`: the scene-frame guard prevents the second eye from adding the
  same origin jump again. Recompiling the new source block in the earlier
  13 km reproduction produces `shift=13000 accepted=1` for both eyes.
- The same commit re-expresses the rate ring with `t -= omega cross shift`.
  Re-running the earlier 16-pair example with the new function removes its
  approximately 9.076 m/frame Y translation error from the first new pair.
  This verifies the previously failing case, not every possible rebase.
- `a86f8b3` and `4e3b074`: the smoke floor and opacity-scaled mask are now
  configurable under `[advanced]`. Defaults remain floor `0.08` and
  reactive strength `0`.
- `a9c3a7a`: a withheld jump can preserve history when the detector reports
  a lasting reference-frame change. This is separate from smoke coverage.
  The large reset counts after 10:26:11 coincide with the pilot entering
  debug views, which invalidate trained history; they are not evidence of
  the station flicker returning during normal rendering.

The 10:25:42 registration interval reports zero body frame-agreement
stand-downs. This is consistent with the pilot's report that the station
now works.

## What the new captures establish

Files are under the Steam product's `edvr_logs/eyes` and `edvr_logs/pool`.
Row numbers below are zero-based positions in the binary draw ledger,
not the separate text census's draw numbers.

| Eye run | Matching ledger | Relevant draws across its 19 populated frames |
|---|---|---|
| `eye_102516_T00..T15.bmp` | `draws_102516.bin`, frames 16580–16598 | 28 ribbon draws; 226 volume draws |
| `eye_102522_T00..T15.bmp` | `draws_102522.bin`, frames 17097–17115 | 38 ribbon draws; **zero** draws of the identified volume VS |
| `eye_102625_T00..T15.bmp` | `draws_102625.bin`, frames 22693–22711 | 28 ribbon draws; zero identified volume draws; **depth debug view** |

The first frame of each ledger is an empty arming frame; each run contains
20 frame headers. The parser checked the complete file length.

The gaps are conspicuous in `102522`, including long dark strips and
straight segment ends. In every populated frame its ribbon appears at
rows 304 and 401, with 600 indices and one instance. The identified volume
VS `203DF51758AADC4D` is absent. That volume therefore cannot by itself
explain the persistent rectangles. The ribbon VS is `5E417E9DF2E7F9E6`,
with PS `BD801F2FB02522EB` in the recorded shader family.

The depth run shows narrow non-magenta trail regions against a magenta
background. Magenta means no scene depth; black means a positive depth
decoded at least about 256 m away, because this debug palette saturates
there. **Black is not a hole or a measured distance.** This run was taken
about a minute later and cannot be overlaid on `102522` to assign every
colour gap to a particular depth pixel.

The graphics log confirms that heat haze is being withheld, with zero
sampled binding-shadow disagreements. This differs from the historical
session in the design journal where a broken shader lookup let haze
through. There is no corresponding evidence of that failure here.

These are treated output captures: `advanced.eye_run_treated = 1`. Both
the `T` crops and the `L0` overview are post-treatment. They do not contain
the pre-DLSS colour needed to distinguish a rendering hole from history
reconstruction. The binary draw ledgers identify draw order and counts;
they do not contain each smoke draw's constant buffers, depth state,
textures, or vertex/index contents. No new full text census appears in
the 10:19 graphics log; the latest complete one available is at 08:02:53.

## Finding: smoke coverage can occlude subsequent game rendering

This is an existing defect left in the branch, not introduced by the new
knobs. `ui_depth.cpp:1822` reissues the smoke draw immediately after its
colour draw, as called by `vscreen.cpp:3065`. It binds `g_savedDsv` at
`ui_depth.cpp:1847`, and `reissueState` at line 957 enables depth writes
with `GREATER_EQUAL`. The smoke uses `kReissueScene`, so this is the live
scene depth target. Restoring the state and bindings in
`uiDepthReissueEnd` does **not** restore the depth contents.

The original ribbon has its depth test disabled. EDVR adds an opaque
depth surface under part of this transparent, additive effect. Subsequent
depth-tested draws behind that surface can be rejected even though their
colour should contribute through the smoke.

The full 08:02:53 census provides a concrete draw-order example:

- Frame 0, draw 487: ribbon, colour `@93`, depth `@81`, `ds=02wA`
  (depth disabled).
- Draws 489 and 493: volume, same colour and depth targets, `ds=17wZ`
  (depth enabled, `GREATER_EQUAL`, writes disabled).
- The next eye has the same ordering and shared targets within that eye.

The new `102516` ledger also places volume draws after the ribbon: rows
258, then 260–270 in frame 16580, followed by the second-eye group.
Thus this hazard applies to an observed sequence, not just an imagined
ordering. However, `102522` has no identified volume draws, so this
mechanism is not established as the complete explanation for its gaps.

**Correction direction:** record smoke coverage/depth in a private
temporal input, and combine it with the completed scene depth for motion
generation. Preserve the game's depth contents for subsequent rendering.
Simply lowering the alpha floor would enlarge the area exposed to this
hazard.

## Finding: the new mask still excludes the fading fringe

At `ui_depth.cpp:394`, `clip(alpha - floorAndStrength.z)` executes before
both the depth output and the new reactive-mask output. Therefore smoke
below the floor receives neither. Above it, the defaults write depth and
mark exactly `1/255`; below it, the background's depth and mask remain.
Animated texture samples can move a pixel across this discontinuity even
when the ribbon geometry itself is stationary in space.

The new opacity-scaled mask also cannot protect those discarded pixels
when its strength is raised. With floor `0.08` and strength `1`, alpha
`0.079` produces no mark; alpha `0.081` produces approximately `21/255`.
Strength `1` is a maximum scaled by alpha, not a blanket full-strength
history rejection over the ribbon. The depth step remains at the same
place. This is a plausible source of visible segment boundaries under
motion, not a pixel-by-pixel diagnosis proven by the current captures.

The installed INI lines 1027–1028 comment out both new options; the log
contains no change announcement for them. With the active UI alpha floor
of `0.50`, the old and new smoke defaults both use `0.08`. Consequently,
installing this branch alone did not change the smoke's coverage or mask.

**Correction direction:** evaluate a separate, smoothly varying reactive
coverage rule for the faint smoke, independently of the threshold for
writing representative depth. Validate representative depth before
tuning this threshold or increasing the area marked as smoke.

## Additional lead: validate the ribbon's depth encoding

The installed ribbon VS disassembly contains:

```text
20: dp4 r1.w, cb0[6].xyzw, r0.xyzw
21: dp4 r2.w, cb0[7].xyzw, r0.xyzw
29: add o4.z, r1.w, l(15.010000)
30: mov o4.xyw, r2.xyxw
31: mov o1.xyz, r2.xywx
```

Thus its raster depth includes `15.01 / clipW`, whereas TEXCOORD1.z
retains clipW. The coverage PS writes `i.pos.z` at line 395 without
checking whether that value has the scene's encoding. The temporal pass
decodes scene depth using `0.025 / depth` in this session (graphics log
line 491). The ordinary volume VS has no corresponding added constant.

A D3D11 WARP reproduction used the **actual dumped ribbon VS** and the
**unmodified current-branch coverage PS**, with a synthetic conventional
reversed-Z matrix, known geometry and constant textures. It produced:

| Synthetic physical distance | Stored smoke depth | Distance decoded by the temporal pass |
|---|---|---|
| 100 m | 0.150350 | 0.166279 m |
| 1,000 m | 0.015035 | 1.662787 m |
| 10,000 m | 0.0015035 | 16.627869 m |

The same GPU test verified that a subsequent same-distance,
`GREATER_EQUAL` colour draw fails after that injected depth and succeeds
without it. It also verified the default `1/255` mask.

**These distances are synthetic, not measurements of the captured trail.**
The smoke's actual per-draw CB0 is missing, and its matrix could contain
a compensating term. The black trail in the depth debug image does not
establish the severe near-depth error in the synthetic example. Capture
CB0 and compare `i.pos.z` with depth reconstructed from TEXCOORD1.z and
the scene projection before deciding whether to remove the offset or
replace the encoding. Do not blindly subtract 15.01 from stored depth:
it was added before perspective division.

## Smallest decisive follow-up

Keep DLSS, `drives_smoke = on`, and the current heat-haze suppression.
Take matching trail views with `fix.temporal_aa_smoke` on and off, using
`advanced.eye_run_treated = 0` so each run saves pre-DLSS `C` crops and a
treated `L0` overview. The toggle disables only EDVR's smoke coverage
pass; the game's smoke colour draws remain.

- Raw holes disappearing with coverage off implicate the injected pass's
  effect on game rendering.
- Clean raw colour but gaps in treated output implicate depth/motion or
  temporal history handling.
- Raw holes in both states require inspection of the game's smoke colour
  draws and other active modifications.

For the depth-encoding lead, additionally capture the ribbon draw's CB0
with its D3D11.1 binding offset, and diagnostic outputs of raster depth,
TEXCOORD1.z, alpha, and mask for the same frame. A debug view alone cannot
recover this information. If comparing temporal AA on and off instead,
force heat haze off in both cases: `auto` otherwise changes that effect
at the same time and confounds the comparison.

Local verification: compiled source reproductions for the two station
fixes passed; installed VS/current coverage PS WARP reproduction passed;
all three binary ledgers parsed to their exact ends. No live-game A/B
was performed by this review.
