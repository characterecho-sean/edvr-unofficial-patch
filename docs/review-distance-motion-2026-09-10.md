# Review: distant blur during motion, 2026-09-10

**Follow-up:** the two reproduced station cases below pass on branch
`claude/fervent-galileo-a4ede2` through `44edb30`. See the
[smoke-voids review](review-smoke-voids-2026-09-10.md) for the new captures
and remaining rendering findings. The original review below records the
state at `3340c87`.

The moving-view problem is not sufficiently explained by the conclusion that
the panels are near the pixel size and NVIDIA smooths them. Two reproducible
coordinate-continuity bugs remain in the station's motion path. Fix those
before judging the residual blur or changing the reconstruction model again.
Neither reproduction proves that these bugs explain all of the pilot's blur.

Reviewed source: `3340c87`. The relevant motion, object-probe and NGX source
files are unchanged since `35e77ff`, the build identified by the completed
06:21 and 06:36 sessions. No rendering code, installed DLL or setting was
changed for this review.

## 1. P1: an origin shift is accumulated twice in a stereo frame

**Confirmed in source and a compiled reproduction.** In
`src/d3d11/temporal_pass.cpp:3440`, each call of `temporalInner` tests the
camera translation for a jump and adds `camMove` to the shared `g_bodyShift`.
`temporalInner` runs for each eye. `chooseCameraRows` chooses the shared rows
once per frame (`:2640`), but does not guard the subsequent accumulation.
Setting `g_bodyShiftFrame` records the frame; it does not stop the second
eye from adding the same jump.

For a 13,000 m shift, with the held object's pair still in the old origin:

| Evaluation | Accumulated shift | Result of the 4,000 m agreement gate |
|---|---:|---|
| First eye | 13,000 m | Accepts the shifted pair |
| Second eye, same frame | 26,000 m | Rejects it |
| Following frame, same old pair | 26,000 m | Still rejects it |

The gate is `bodyFrameAgrees` at `temporal_pass.cpp:3603`. It checks either
unshifted or shifted agreement; neither can accept the doubled shift in this
example. The second eye loses the station's motion correction first, and
subsequent frames can lose it until a new pair agrees unshifted. The station
then uses camera motion alone while its own rotation continues.

The reproduction extracted the **unchanged jump block and agreement lambda**
from this checkout, supplied their scalar state, and compiled with MSVC:

```text
shift=13000 accepted=1
shift=26000 accepted=0
```

**Correction:** accumulate accepted origin changes once per scene frame and
let both eyes consume the same result. Keep the pair's origin identity
explicit, rather than treating a shared mutable shift as eye-local state.
Test both eye orders, an old pair surviving several frames, a fresh pair,
and a jump followed by its return. The current synthetic case fails before
any image-quality judgement is needed.

## 2. P1: the rate filter mixes translations from different origins

**Confirmed in source and a compiled reproduction.**
`src/d3d11/object_probe.cpp:1651` retains the sixteen-pair rate ring while
the same body remains continuous. It inserts `tf / dt` from the newest pair,
then `rateMedian` (`:952`) takes each translation component's median.
There is no origin attached to a `RatePair`, and no rebase of the old
translation samples. The ring is cleared for a missing/old body or worker
reset, not for a continuing body whose coordinate origin changed.

For the same physical rotation, changing coordinate origin by `s` changes
the affine translation:

```text
t_new = t_old + (I - R) s
```

These are different coordinate representations of the same motion, not
noisy estimates to average together. The code correctly uses this relation
to carry an OLD published body in `temporal_pass.cpp:3674`. However, once a
NEW pair is published, its `camPos` and grid are in the new origin while its
filtered translation can still be dominated by old-origin samples. The
unshifted agreement at `:3615` then clears the render-side correction.

Using the actual `rateMedian` routine, a steady 0.08-degree turn over a
22.2 ms pair and a 13 km perpendicular origin shift leave a **9.076 m
translation error per 11.1 ms frame** for the first eight new samples in
the synthetic case. Its pair translation is below the code's 20 m gate.
With pairs approximately every eight frames, the filter can retain the
wrong translation for about 0.7 seconds at 90 Hz even though the body is
claimed and its reported fit is good. At 10 km, a 9 m transverse error is
large enough to matter at this render resolution.

**Correction:** re-express every retained sample in one origin before
filtering, or restart the filter on a verified origin change. Publish the
grid, camera/origin and motion in that same coordinate frame. Rebase
invariance is the test: translating the coordinate system must leave the
projected motion of the same physical surface unchanged, including the
first newly published pair. A low fit residual alone cannot verify this;
it measures the new raw fit, before the mixed-origin median.

## What the Steam data establishes

Install inspected:
`C:\Steam\steamapps\common\Elite Dangerous\Products\elite-dangerous-odyssey-64`.
The 07:02 game process was running; its graphics and VR logs were zero bytes
when inspected. Its later pool files existed, but there was no completed
07:02 log or matching new eye run to correlate with them. The completed
06:21/06:36 logs and their captures provide the usable motion evidence.

The installed configuration selects DLSS, preset K, object motion on,
mover mask off, fovea off, treated eye runs, and automatic texture bias.
The 06:36 graphics log confirms **2862x2826 input to 4404x4348 output**,
quality mode, preset K, and **-0.62 mip bias**. Both eyes create the expected
features (`edvr_gfx_20260910_063650.log:112`). The old every-evaluation
history-reset bug is not present: substantial stationary intervals have
zero resets, and the source maintains `dlHaveHistory` separately.

Selected twenty-second intervals:

| Graphics log / interval end | Camera translation reported, m/evaluation | Body gate refusals, scene frames | Origin carries, scene frames | History resets, eye evaluations |
|---|---:|---:|---:|---:|
| 062129 / 06:23:29 | 87.8695 | 17 | 0 | 0 |
| 062129 / 06:24:09 | 0.0020 | 0 | 0 | 0 |
| 062129 / 06:26:09 | 5.4612 | 17 | 0 | 2 |
| 063650 / 06:39:51 | 5.6805 | 14 | 0 | 2 |
| 063650 / 06:40:11 | 3.3985 | 7 | 0 | 4 |
| 063650 / 06:51:51 | 4.3343 | 16 | 0 | 6 |

The translation statistic includes rejected camera changes and origin jumps;
it is **not a speedometer**. Camera-drop and reset counters increment per
eye, whereas the body gate counter is guarded per scene frame. Their
shared word "frames" in the log is misleading.

These observations are consistent with an intermittent motion-path failure,
but are not an event-by-event trace. In particular, the 06:26:09 interval
contains 17 body refusals and **zero new camera-jump evaluations**. The
forty-sixth-session note's association with origin jumps must remain a
candidate explanation; the existing summaries cannot locate each refusal
or distinguish a prior bad shift from another camera mismatch.

The saved 06:39:22 run contains 20 pool frames, 19 instance streams and
30,912 draw rows. Re-running the ledger with the three verified pool-reader
shaders shows drawn ring records turning about 0.0466, 0.0889 and 0.0400
degrees on its first three consecutive transitions. The pool does contain
nonuniform steps. The ledger does **not** save the per-frame applied vector
or timing, so this does not establish how accurately the rate predictor
handled those particular transitions.

I disassembled the Steam shader captures for `436193B352A2897E`,
`EB5234DB6ADB491D` and `DE545DC8EE4FBB87`. They retain the t33/336-byte pool
and t38/48-byte palette declarations. The first shader's position path
loads the record position, subtracts `cb1[275]`, applies the decoded
quaternion and scale, and projects through `cb1[270..273]`. This supports
the pool-based path for those draws; it does not revive the earlier
counter-rotation or bone-spin explanations for all station pixels.

## 3. P2: switching to DLAA alone does not test more input detail

The forty-fourth-flight advice in `per-object-motion.md` says changing
`temporal_aa` to `dlaa` makes the panel lattice's input period half again
larger. The implementation does not do that.

`src/openvr/temporal_aa.cpp:544` sets a larger **output** size only for
DLSS. DLAA keeps the size of the frame Elite submitted; it does not raise
Elite's HMD Quality. At the current 0.650 render fraction, switching only
the EDVR mode gives DLAA at the reduced input size, followed by compositor
scaling, rather than native-input DLAA.

For a test of input sampling, raise **Elite's HMD Quality to 1.0** and use
DLAA, then verify the actual dimensions in the graphics log. At unchanged
runtime settings that would move this input toward 4404x4348: about 1.54
times the samples on each axis and 2.37 times as many input pixels. That is
a meaningful resolution A/B, with a real rendering-cost increase. It still
does not fix an incorrect motion transform.

## What is still unproved, and the next useful measurement

The 06:26:17, 06:39:22, 06:39:38 and 06:51:55 runs hold sixteen **treated**
crops and one full treated image each. Raw sequences on disk are 05:46:36,
06:11:22 and 06:11:33, from earlier views. The moving captures show station
detail affected differently from the cockpit, but there is no simultaneous
raw/treated sequence for them. Comparing those runs across different
viewpoints cannot isolate reconstruction blur from input sampling.

The current capture code deliberately chooses raw **or** treated
(`temporal_pass.cpp:2424`, `:2383`). Its fixed 1400-pixel crop also covers
different angular areas at input and output resolution. An improved capture
should save both for the same frames and matching image area, plus depth,
motion vectors, object ownership, the actual applied matrices, source-pair
frame/origin, reset flags, and frame timing for both eyes. Batch the writes
after capture; the existing dump itself can hitch.

There is a further predictor limit worth measuring after the two fixes:
the body uses a sixteen-pair median and eased submission-clock interval,
and replaces intervals outside 5..50 ms with 11.1 ms
(`temporal_pass.cpp:3659`). Long frames and irregular simulation steps can
therefore be underpredicted. Present data cannot separate that error from
the reconstruction model's response to a moving near-pixel lattice.

The stationary sharpness scores in the design journal do not establish
motion fidelity, and an aggregate percentage of claimed pixels cannot
prove that a particular panel has correct ownership. NVIDIA's integration
guide also requires the frame inputs and constants to correspond to the
same evaluation; inspecting these inputs together is the appropriate
validation boundary ([NVIDIA Streamline DLSS guide, section 7](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md#70-add-dlss-to-the-rendering-pipeline)).

Recommended order: fix and test the two origin-continuity bugs, capture a
stationary and moving sequence with simultaneous inputs and outputs, then
compare K/J and native-input DLAA while keeping view and motion comparable.
The user's J result is useful evidence about a tradeoff; it does not clear
the motion inputs. Mip bias toward zero may reduce shimmer by discarding
fine texture detail, so it is not the first remedy for lost detail in motion.

Review-only artifacts are under `build/review_motion`: the source-extraction
script, compiled reproduction, interval JSON, shader disassemblies, ledger
output and capture inventory. The reproduction exited successfully after
asserting both failure cases. No full game build or live visual A/B was run.
