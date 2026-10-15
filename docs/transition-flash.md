# The one-frame flash at transitions

## Status

- **State (2026-10-13): the flash is fixed in the engine's own frame order, and
  that is the only mechanism left.** The exact withhold is ALWAYS ON with no
  setting: the camera driver's consume that reads a reset mailbox arms an event,
  and the one frame drawn from it is held back (the compositor keeps the
  previous frame). Flight 095137: every transition flown, no flash seen; 12
  events, 13 frames held, no misses, no false holds, DLSS history kept. When the
  game build is not the one EDVR knows, the consumer hook does not install, or
  the camera-buffer tap cannot deliver, the log says which and that NO frame is
  held: the old fallback detector was removed 2026-10-13 (last entry). Full
  story: `docs\design-transition-flash-engine-fix-2026-09-23.md` (Status first).
- **Retired 2026-10-13 (Sean approved), quoted:** `fix.transition_flash` (`1`
  on, `0` off, needs a restart; menu row "Stop the one-frame flash on jumps");
  `transitionEnabled` in the frame ABI is now always 1. An old line is carried
  by the installer as "no longer used", named by the config audit, read by
  nothing. Already gone earlier (124b6d09, 2026-10-09): 
  `advanced.transition_flash_units`, `_speed_factor`, `_max_consecutive`,
  `_repeat_percent`, `_radius_tolerance`, `_park_units`, `_dwell_frames`,
  `_burst_limit`, `_burst_window`, `_separation`, `_drift_pct`, `_resubmit`,
  `_diagnostics` and `_eye_base`; `_prevent` went in 68bddaaa (2026-09-29).
- **Kept, on purpose:** `hotkey.dump_camera` (Pause) and its dump, which now
  prints the temporal pass's camera history only.
- **Retired 2026-10-14 (Sean approved), quoted:** `advanced.camera_buffer_bytes`
  (default `5376`) and `advanced.camera_buffer_offset` (default `1100`): they
  placed the camera origin in the game's scene constant buffer and gated only
  the camera-buffer tap. Now constants in `glitch_frame.cpp` (the engine fix's
  own `kSceneCBBytes`, `kSceneCBOriginFloat`) with a `static_assert` on the
  fit. The rigs switch the tap off with the environment variable
  `EDVR_TEST_NO_CAMERA_TAP=1`, not a key. Same day, `vscreen.cpp`'s dead
  "counted as the scene" promotion (`sceneW`/`sceneH`) went; last entry.
- **What the bad frame is:** the HMDCamera (the head camera) is deactivated one
  frame and re-activated after the camera driver's consume, so the mailbox the
  driver composes the eyes from is still its reset value; the eye origin
  `cb1[275]` collapses to the head pose alone for that one frame. Correcting the
  frame in place was tried and flashed (the bad frame carries at least five
  other wrong structures); holding it does not.
- **Cost:** an estimate, not an A/B measurement: a 64-byte compare per consume
  and the idle path of the camera-buffer Unmap; the detector's per-draw and
  per-Map work is gone, and a build that cannot arm pays only the idle test.
- **Open:** issue #34 (second and later jumps) was closed on timing and never
  re-measured on the engine fix.
- **Ruled out:** listed at the end of the design doc.

*Frontier issue [37825](https://issues.frontierstore.net/issue-detail/37825) —
"VR - Random, glitchy frame(s) appearing when entering orbital glide on a
planet"*

This is a write-up of what the bug actually does, measured rather than guessed,
and what EDVR does about it from outside the game. It is written for whoever
might fix it properly, inside the game, where it can be fixed properly.

**Short version: one frame per transition is rendered from the wrong viewpoint.**
It is not a dropped frame, not a repeated frame, and not a camera that jitters.
For a single frame the world is drawn from a place the player is not, and the
frame after it is correct again.

---

## The symptom

Jumping, dropping out of supercruise, entering orbital glide, or closing the
galaxy map occasionally shows a single frame of somewhere else. Reports in the
tracker describe it as "looking behind the ship for an instant", "the target
destination reticle appearing briefly in the cockpit", and "a subliminal image".
All three are consistent with one frame drawn from a different origin, and none
of them is consistent with a stutter.

On a monitor this is a blink. In a headset it is not. A headset shows the frame
to both eyes with full stereo commitment, and the visual system reads a
single-frame viewpoint change as self-motion rather than as an image artifact.
That is why this is a VR-specific complaint about a renderer behaviour that is
presumably not VR-specific at all.

## What is actually happening

Every draw call in a frame was attributed to the camera in effect when it was
issued, across three consecutive frames — the one before the flash, the flash,
and the one after:

| Frame | Draws into the eye textures | Dominant camera position | Share of draws |
|---|---|---|---|
| Before | 289 | about **885 units** from origin | 96.5% |
| **The bad frame** | **618** | **the origin, under 1 unit** | **94.0%** |
| After | 405 | about **59 units** from origin | 71.4% |

The camera is not missing on the bad frame and restored afterwards. It is at a
*different* distance on each of three consecutive frames: 885, then 0, then 59.
Three frames, three coordinate spaces.

The bad frame also contains a second camera at (+2424.62, +1257.39, −282.36),
about 2746 units out, whose forward vector (+0.951, +0.288, −0.111) matches the
normal camera's (+0.951, +0.288, −0.112) to three decimal places. The same
orientation at roughly 46× the distance is what one view expressed in two
different reference frames looks like.

**The reading this supports** — and this is inference, not measurement — is that
the frame is issued at the moment the reference origin has been re-based but the
camera has not yet been placed into the new frame, so the world is rendered from
that origin rather than from where the player is.

## How consistent it is

Steady cockpit flight is very consistent: a median of 280–289 draws into the eye
textures, with the busiest ordinary frame reaching about 375–405. Across 900
consecutive ordinary frames, not one exceeded 1.8× the median.

Exactly one frame per transition is an outlier, and it lands precisely where the
flash is seen:

| Session | Median of 900 frames | Outlier frame |
|---|---|---|
| 1 | 287 | 626 |
| 2 | 285 | 624 |
| 3 | 289 | 609 |
| 4 | 283 | 589 |
| 5 | 287 | 618 |

Five low wake jumps, five sessions, one frame each time at roughly 2× the
surrounding median.

The extra draws are **not the scene being drawn twice**. Across the 56 vertex
shaders present in both the bad frame and a normal one, the median ratio of draw
counts is exactly 1.00. The excess comes from five specific shaders that normally
draw 2–10 times each and instead draw 133, 62, 62, 48 and 25 — about +312 draws.

## What it is not

- **Not a camera that jerks.** Across a ten-second window the largest
  frame-to-frame change in the sampled camera's orientation was 0.82°, with no
  spikes.
- **Not a dropped or repeated frame.** Those draw *less*. This draws more.
- **Not specific to one transition type.** It was reproduced on low wake jumps;
  the tracker has the same report for orbital glide entry, supercruise entry and
  exit, and hyperspace. All are transitions, which fits a frame issued while the
  renderer is between two states.

## What a fix inside the game would look like

The frame is already wrong by the time anything outside the game can see it, so
everything below is about ordering rather than about rendering.

The useful question is: **what re-bases the reference origin, and can a frame be
issued between that and the camera being placed in the new frame?** If those two
are separate steps and a present can land between them, that is the bug, and the
fix is to make the pair atomic with respect to frame submission — either by
doing both before any draw for that frame is issued, or by deferring the re-base
to a frame boundary.

If that ordering is already guaranteed, then the second camera in the bad frame —
the one at 2746 units with the matching orientation — is the thread to pull,
because something in that frame clearly does know where the view should be.

## Reproducing it

Any frame debugger will do. The measurements above came from recording, per
frame, how many draw calls reach the submitted eye textures, keeping ten seconds
of that in memory and writing it out on a keypress after seeing the flash. The
bad frame is then trivially identifiable as the one outlier, and every
constant-buffer write in it can be attributed to the draws that read it.

The one thing that matters methodologically: **buffer the frame and keep it
retroactively**. A recording started after the flash is seen has already missed
it, and a recording that starts mid-frame will make the bad frame look like it
draws less than it does. An earlier version of this analysis wrongly concluded
"the scene is rendered twice" from exactly that mistake — a partial frame
compared against a whole one.

Hardware: PC, SteamVR, reproducible every time. One machine, one headset, one
game build. It is a reproduction, not a survey.

---

## What EDVR does about it

**The shipped fix (since 2026-10-09) holds the one bad frame, exactly.** It does
not look at the picture. The game's camera driver reads a small matrix before it
composes the eye views; on the bad frame that matrix is still its reset value,
and EDVR sees that read, waits for the frame drawn from it (the camera buffer
for the first eye draw with a head-only eye origin) and withholds it. SteamVR
shows the previous frame in its place; EDVR tells the temporal pass the camera
stayed, so its history is kept. One held frame per transition, nothing written
into the game, no guessing from camera distances.

**The rest of this section describes the older DETECTOR, REMOVED 2026-10-13**
(it was only the fallback when the engine fix could not arm, an Elite build
EDVR does not know); it stays as the record of what was built and measured. It
**detected the bad frame while it is still being drawn and hands SteamVR a copy of
the previous frame in its place** -- EDVR keeps a copy of the last frame it
actually forwarded, and a caught frame becomes an on-time submit of that copy at
the current pose. One repeated frame, no stall, always the game's own picture;
EDVR draws nothing.

When no copy is usable -- the first frames of a session, or the eye textures
changing size -- it falls back to the old mechanism: declining the submit
entirely, which SteamVR answers by reprojecting the previous frame itself. That
fallback costs about 80 ms while the compositor waits for a frame that never
comes, which was the price of *every* caught frame before 0.7.0 and the source
of the judder some configurations felt.

Detection works by watching the viewpoint the game is drawing from and comparing
it against a straight-line prediction from the previous two frames. Ordinary
flight sits about 30 units off that prediction; the bad frames measured here were
9,900 to 24,000 off. The threshold is 2,000, which is far from both.

Two details do most of the work:

- **Predict, do not difference.** Comparing against the previous position makes
  travelling fast look identical to glitching. Comparing against where the
  viewpoint was *heading* does not.
- **Ask whether it came back.** A bad frame leaves the path for one frame and
  returns. Arriving at a station or opening a map moves the whole coordinate
  system and *stays* there. Without this test, a station arrival produced bursts
  of eleven and twelve withheld frames — under the pre-0.7.0 mechanism, most of
  a second of stall, which was worse than the flash it was removing.

**Known limits, stated plainly.** The viewpoint EDVR can see is the furthest-out
camera in each frame, because Elite renders the main view camera-relative and its
world position never reaches a constant buffer. Which camera that is depends on
which passes ran, and which passes ran depends on how heavy the frame was. So the
signal is partly a measure of scene complexity rather than of where the view is.

That is not a small caveat, and on a planet surface it was the whole story.
Flying low over terrain the frame alternates between two shadow cascades, and the
distance between them reads as an enormous jump — measured at about 568,000 units
against genuine transitions of 2,300 to 24,000, thirty-eight times in eight
minutes while the ship flew straight. All of them were withheld, about three
frames apart, which is felt as rhythmic judder rather than as anything to do with
flashes; then the runaway guard turned the fix off for the rest of the session
and the real flashes came back with it.

**The discriminator is repetition — of geometry, in four shapes.** A fixed
distance between two render passes recurs at the same size indefinitely; a real
transition does not, because real motion varies. Of fourteen genuine transitions
measured in one session the closest pair were 5.7% apart, while the cascade pair
repeated within 1.5%. Field sessions then found three more shapes the same
principle covers, and each is recognised separately:

- **A repeating jump size** is a fixed gap between two passes — but a jump size
  can also repeat across two *transitions* to the same reset point, so this rule
  only acts after a size has cost three frames inside a minute
  (a fixed 2% tolerance).
- **A recurring distance** is a camera orbiting the view at a fixed radius; a
  camera that *sits* on a distance for a third of a second is the view itself,
  and that distance is never excused — or stops being excused, if it was
  (0.5% radius tolerance, 20 frames).
- **A recurring landing point** is a parked camera being resampled
  (64 world units).
- **A steadily climbing size, landing near its last landing,** is one camera
  drifting away from the view (10%).

Above all of them sits a **burst governor**: more than three caught frames
inside sixty stands the fix down for two seconds, whatever the cause — the fix
may never cost more than the thing it hides, including for storm shapes nothing
above recognises yet (three catches in sixty frames).

The counts are reported, split by rule, in a `transition flash so far:` line,
printed roughly every twenty seconds and only when one of them has moved — so no
such line at all means the fix never fired. It is deliberately not left to the
end of the session: the log has no end. A game's `d3d11.dll` is never unloaded,
only terminated with the process, and the teardown path that runs then is
restricted to what is safe when every other thread is already dead. A summary
written there would be a summary nobody ever saw, which is what the previous one
was for five sessions of debugging.

This matters beyond the judder. Every caught frame costs the detector its next
frame or two while it rebuilds its prediction, and arrivals and wakes churn the
pass mix for *seconds* — so a cascade flip firing shortly before a genuine bad
frame spent the detector's shot early and the real flash was shown. Some share of
"detected and let through" reports is likely to have been that. The recognition
rules above exist to keep those shots loaded.

There is still a hard cap of two catches in a row, the burst governor above,
and the guard that switches the whole thing off if it ever starts firing
continuously.

**Both eyes of a frame now get the same answer.** The decision is taken once, at
whichever eye the game submits first, and the second eye follows it. Previously
each eye read the flag independently and the flag can legitimately change while a
frame is being drawn, so a change landing between the two submissions would have
shown one eye the current frame and the other the previous one — a one-frame
mismatch between the eyes, which feels like exactly the thing this fix is for.

This is a workaround with a measurable failure mode, kept because the alternative
is watching the flash. It is not a substitute for fixing the ordering.

## The one at the external camera is Elite's, and cannot be hidden

Entering and leaving the on-foot external camera shows the world from the wrong
place for a frame or two — usually described as being briefly under the terrain.
It is not this fix misfiring: it happens identically with `transition_flash = 0`,
where EDVR withholds nothing at all.

It also cannot be worked around from out here, and that is worth recording
because the obvious idea looks sound. Since the player's own key press announces
the transition, the frame before it is known good, so holding that frame across
the transition should hide the whole thing. It was built and measured, and it
fails for a reason nothing in the design anticipated: **declining a frame stalls
the game.** The compositor waits for a submit that never arrives — which is where
the 80-odd milliseconds per withheld frame goes — and Elite does not begin the
transition until its frames are reaching the compositor again.

So the hold does not cover the transition. It postpones it. Measured at a hold of
ten frames, the ten held frames still show the ordinary pre-transition view; the
transition begins four frames *after* the hold releases, and the bad frame lands
five frames after that. Holds of 18, 30 and 90 frames each did the same thing.
There is no length that works, because the thing being outlasted is waiting for
the hold to stop.

**0.7.0 changed the premise of that measurement, and honesty requires saying
so.** The refutation rested on declining-stalls-the-game — and a caught frame
is no longer declined, it is replaced with a copy submitted on time, so the
game keeps receiving frames and should keep running the transition underneath
the hold. Whether a hold now covers the bad frames instead of postponing them
is an open question that has not been re-measured; the switch still ships at 0,
and the measurement above stands as the record of why it was 0 before.

The switch survived as `hold_frames_on_external_cam`, defaulting to 0, until
2026-10-07, when it was removed with the old Explorer Cam route whose camera
key pressed it. The history stays here so the idea is neither rediscovered from
scratch nor dismissed on a measurement whose premise has moved.

**If you see a flash that got through:** press **Pause** immediately, then quit
and send `edvr_logs\`. That writes the temporal pass's recent camera history and
says whether the hold was armed. A `transition flash: frame N held` line near the
moment means a frame was held and the flash got through anyway; no such line
means nothing was held. Those are different bugs and the log is the only thing
that distinguishes them.

---

## 2026-10-13: the key and the fallback detector are gone

Sean decided: the engine fix's hold is always on, and the independent detector
that ran when it could not arm is removed. Nothing in a flight that mattered
used the detector: it was dormant whenever the engine fix was armed (every
flight since 095137), and its twelve tuning keys had already gone in Build 2d.

- **Removed key, quoted:** `fix.transition_flash` (`1` on, `0` off, needs a
  restart; menu row "Stop the one-frame flash on jumps"). Its `0` path did two
  things, both gone: the engine fix's install (`transitionFlashEyeBaseConfigure`
  read it once) and the provider's `transitionEnabled` word that gated the
  compositor's hold in `native_frame.cpp` (`(transitionEnabled && marked) ||
  held`; now `marked || held`, the ABI field stays and is always 1).
- **The install is gated by the profile, not a key.** In the flat profile the
  old key was refused by the profile allow-list and read false, so the engine
  fix never installed there; it still does not (`runtimeVrProfile()` is the gate,
  and a flat launch that switches to VR installs it then).
- **Removed code:** the detector in `glitch_frame.cpp` (2,847 lines to 189:
  the jump chain over the camera position, the separation memory, certified
  orbits and parks, the pool-coherence cross-check and its scene-draw and
  pool-recording hooks in `vscreen.cpp`, the camera-history ring and its dump
  `dumpCameraRing`, the `GlitchTuning` constants, the churn instruments),
  `glitch_scene.h`, `unmarkGlitchFrame` (only the detector withdrew a mark), the
  `countForFlashFix` eye-draw counting and the dead `sceneDrawsThisFrame` it
  summed, and `tools\glitch_test`'s detector cases with their three field
  corpora. What the engine fix used stays: the camera-CB tap
  (`glitchFrameObserve`), the event window, the hold (`glitchFrameEngineFixHold`)
  and the verdict, the frame counter, the tap-availability check.
- **Verified detector-only before cutting (origin/main 7e44b86c).** Armed, the
  detector was dormant on every path: `glitchFrameObserve` returned at
  `engineObserveIdle()` (glitch_frame.cpp:1604) and its jump chain at
  `engineFixDormant()` (1700), pool and scene-draw recording stopped at
  `poolRecordingWanted()` (1964, 1969, 2032), the boundary at 2093,
  `glitchFrameNeedsEyeDraws()` answered false (1586, so `countForFlashFix`
  counted nothing), and the dump refused (2593). `glitchFrameSceneGeometry` and
  every `GlitchScene*` type had no caller outside the detector and its rig.
  The twelve tuning keys and `_diagnostics` had no reader at all (Build 2d);
  `GlitchTuning` (glitch_frame.h:33) held their old defaults for the detector
  only. What the engine fix used and kept: the event window (1386), the Armed
  report (1396), tap availability (1412), the hold (1435) and the verdict
  (1447), `glitchFrameWantsBuffer` (1589), `glitchFrameObserve`'s finite check
  and tap call (1597-1645) and the boundary's frame counter (2085).
- **`dump_camera` (Pause) stays.** Its first line is now "camera history dump:
  the temporal pass's recent camera submissions follow ... The transition flash
  fix is armed / NOT armed", then `temporalPassDumpHistory`'s TEMP and TCAM
  rows. `dump_camera_on_external_cam` was already retired 2026-10-07 with the
  old Explorer Cam route (config_test's `kRetiredKeys`); nothing reads it.
- **Stale lines are harmless:** nothing reads `fix.transition_flash`; the config
  audit logs "N line(s) name settings this build does not read: ... a typo or a
  retired setting -- those lines do nothing" once per session; the installer's
  merge carries the line under "this version no longer uses it" (installer_test
  pins it). config_test holds the qualified name out of every source and the
  shipped file (`kRetiredExactKeys`, because the bare name begins the engine
  fix's file names).
- **A build the engine fix does not recognise now has no transition
  protection.** The log says `engine fix NOT armed (<why>) ... No frame will be
  held`. This is the price of removing the fallback, and was accepted.
- **Tests:** `glitch_test` is rewritten for the tap (nomination until Armed, the
  window gating the hand-over, the frame counter, the geometry checks with a
  stale `fix.transition_flash` line, a hold's mark and verdict);
  `native_temporal_test` runs the whole glue chain with a stale `0` in the
  config and ends with the real install path run with no key, a stale `0`, a
  stale `1`, the flat profile (installs nothing) and a switch to VR (installs);
  `native_frame_test` marks a frame under four stale values and expects a jump
  hold each time; `config_test` and `installer_test` hold the key out and pin
  the carry-over.

---

## 2026-10-14: the camera-buffer keys and the scene-count promotion are gone

Cleanup on top of the flight branch (Sean approved all three parts; part 3 is
in `docs\design-flat-camera-integration.md`).

- **Removed keys, quoted:** `advanced.camera_buffer_bytes` (default `5376`, the
  size of the scene constant buffer the tap picks out in Map/Unmap) and
  `advanced.camera_buffer_offset` (default `1100`, the float index of the eye
  origin in it). Both were read only in `installGlitchFrameFix`. A wrong pair
  could only switch the hold off, with a log line; nobody had a reason to move
  them, and the engine fix (`transition_flash_eye_base_core.h`) already carried
  the same two numbers as `kSceneCBBytes` and `kSceneCBOriginFloat`. The tap now
  uses those, and `(1100 * 4 + 12) <= 5376` is a `static_assert`. The template's
  commented lines are gone. The log line `camera-buffer tap ready: reading float
  1100 of the 5376-byte constant buffer ...` still prints at install; the two
  "tap off: ... does not fit" lines went with the keys.
- **Retired-key handling:** both names are in config_test's `kRetiredKeys`, so
  the shipped file and every source are held clear of them. A stale line in a
  user's ini is read by nothing; the config audit logs it as a line this build
  does not read, and the installer carries it under "no longer used".
- **The test switch.** `test_openxr_transport.py` ran the DLL "transport-only"
  (vScreen takes no Map/Unmap hooks and has no frame boundary, which is how the
  Explorer Cam tick is proven not to ride vScreen) by writing
  `camera_buffer_bytes = 0`. The replacement is the environment variable
  `EDVR_TEST_NO_CAMERA_TAP=1`, read once in `installGlitchFrameFix` and honoured
  only when it is exactly `1`. Chosen over an ini key because an ini key is
  something a user can set and a stale line can carry; over a test hook
  function because the transport fixture runs the real d3d11.dll in a child
  process, where only the environment crosses. The fixture passes it with
  `subprocess.run(env=...)`; glitch_test sets it in-process and also pins `0`,
  `11` and the empty string as not switching. When honoured the DLL logs
  `camera-buffer tap off: EDVR_TEST_NO_CAMERA_TAP = 1 (the test rigs' switch)`.
- **The scene-count promotion (vscreen.cpp), removed.** Proof that nothing read
  it, by symbol (all at `src\d3d11\vscreen.cpp` before the edit): `sceneW`/`sceneH`
  were declared at 857, written at 1095 (`eye_render_size = off` zeroed them),
  1105 (a pin), 6276 (the shaped promotion, equal to `renderW`) and 6323 (the
  dominance-only promotion), and read at 6253 (the promote-once latch), 6390 (the
  low-peak notice's gate) and 6676 (a once-only log line). No other file names
  them (`grep sceneW` over `src` finds only vscreen.cpp and unrelated locals in
  flat_runtime.cpp). No draw path added their draws to `eyeDrawsLastFrame` or
  `eyeDrawsMax`, and no accessor in `vscreen.h` exposed them, so "what Explorer
  Cam and the camera scan read is the two added together" was false once the
  old detector's count went. Removed: the fields, `sceneSourceNoted`, the
  dominance-only branch and its `counted as the scene` log, and the
  `the eye-draw counts above are draws into the ... eye textures` log (also
  wrong after a shaped promotion, which makes the target count as an eye).
  The latch is `renderW` now: in an auto session it is set only by the shaped
  promotion, a pin or `off` turns `renderAuto` off, and clearing a pin zeroes it.
- **Kept, because it is read:** `noteSceneCandidate`, `cands[]`, `thisFrame` /
  `bestFrame` and the shaped promotion. They feed `renderW`/`renderH` (the
  measured render scale), which `targetIsEyeSized` reads at 1216 and 5529, and
  `g_renderBelowEye` (the VR supersampling notice). That is live and flown; it
  is not the dead part. `vScreenSceneCandidateDraws` and the
  `edvr_selftest_scene_draws` export are build.bat's counting check. The
  busiest non-shaped target is now named in the low-peak notice's second line
  ("The busiest of them is WxH, with N draws in one frame ... not adopted")
  where the removed log line used to say it; before, that notice stayed silent
  in that case.
- **Behaviour changes, all logs or a latch:** the dominance-only log and the
  scene-source log are gone; the low-peak notice can now speak in the case the
  dominance branch used to silence; a non-shaped busy target no longer blocks a
  later shaped promotion (the old latch was set by either).
