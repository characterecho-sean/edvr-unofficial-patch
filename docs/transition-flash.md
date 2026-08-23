# The one-frame flash at transitions

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

Nothing that should be mistaken for a proper fix. It **detects the bad frame
while it is still being drawn and hands SteamVR a copy of the previous frame in
its place** — EDVR keeps a copy of the last frame it actually forwarded, and a
caught frame becomes an on-time submit of that copy at the current pose. One
repeated frame, no stall, always the game's own picture; EDVR draws nothing.

When no copy is usable — the first frames of a session, or the eye textures
changing size — it falls back to the old mechanism: declining the submit
entirely, which SteamVR answers by reprojecting the previous frame itself. That
fallback costs about 80 ms while the compositor waits for a frame that never
comes, which was the price of *every* caught frame before 0.7.0 and the source
of the judder some configurations felt. `transition_flash_resubmit = 0`
restores it as the only behaviour.

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
  (`transition_flash_repeat_percent`).
- **A recurring distance** is a camera orbiting the view at a fixed radius; a
  camera that *sits* on a distance for a third of a second is the view itself,
  and that distance is never excused — or stops being excused, if it was
  (`transition_flash_radius_tolerance`, `transition_flash_dwell_frames`).
- **A recurring landing point** is a parked camera being resampled
  (`transition_flash_park_units`).
- **A steadily climbing size, landing near its last landing,** is one camera
  drifting away from the view (`transition_flash_drift_pct`).

Above all of them sits a **burst governor**: more than three caught frames
inside sixty stands the fix down for two seconds, whatever the cause — the fix
may never cost more than the thing it hides, including for storm shapes nothing
above recognises yet (`transition_flash_burst_limit`).

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

**A run has to stay in one place.** Keeping the prediction across a withhold is
what makes a two-frame glitch catchable, and it costs this: the frame *after* a
withhold is judged against a path the view may have already left for good. At a
genuine change of reference frame it has. The first frame of the new frame of
reference is a perfectly good frame reading thousands of units off a dead path,
and the second withhold is spent hiding it. The did-it-come-back test exists for
exactly that and cannot help, because it is deferred while the run is still
marking — so the rebase is recognised one frame too late, every time.

Almost everywhere that extra frame looks like its neighbours and nobody sees it.
At hyperspace entry the picture changes completely between them, so the player is
held on the pre-jump image for one frame *after* the game has cut to the tunnel
and then snapped to it. That is a flash the fix created rather than one it hid,
and it is what a 2026-08-23 field report was about.

So a run only continues while it stays put. The distance from the first withheld
frame of a run to the second, across all seven runs in that session:

| run | distance |
|---|---|
| f17777→78, f19089→90 | 0 — two frames at a byte-identical position |
| f20501→02 | 15 |
| f21664→65 | 44 |
| f21380→81 | 123 |
| **f22066→67** | **10,114** — hyperspace entry |
| **f23100→01** | **13,261** — hyperspace arrival, thirteen seconds later |

Two orders of magnitude between the classes, and both members of the upper one
are the reported bug — the two ends of the same jump, each costing a good frame.
The rule fires on two runs in seven.

The threshold is **`transition_flash_run_units`**, its own setting. It was
`transition_flash_units` in the first version of this, on the argument that "the
second frame is itself a jump away from the first" is the question the detector
already asks of every frame — which is still the right way to think about it, but
the two knobs pull opposite ways. Raising `transition_flash_units` withholds
*less*; raising this one withholds *more*, because runs continue more readily.
The advice a few lines up, to raise `transition_flash_units` when far more frames
are being withheld than you made jumps, would have been half wrong.

It is a fixed floor rather than the speed-scaled trip, and the argument against
that is worth recording because it is not weak: `trip` scales with speed
precisely because a camera legitimately gains thousands of units per frame during
a jump, so at supercruise speeds two samples of the *same* wrong viewpoint could
exceed a fixed 2,000 through ordinary motion alone — which is where the flash
lives. Against it: all seven runs above come from frames stepping 10 to 40 units,
so there is no measurement of the fast case either way, and a speed-scaled radius
would switch the rule off during exactly the transitions it exists for. The floor
ships, the knob is separate so it can be moved, and this paragraph is here for
whoever gets the measurement.

Letting that frame through also means something is now known a frame earlier —
the view has moved — so the change of reference frame is declared there rather
than waiting for the run to hit its cap. **Declared, and then confirmed.** One
frame is not enough evidence: when the frame really is the new reference frame
the conclusion is right, but when it is a heavier render pass reporting a
further camera it is wrong, and being wrong costs the 1330 ms stand-down in
which nothing can be withheld. So the stand-down is armed provisionally and the
next frame either confirms it or takes it back, restoring the path and the
stand-down together. Adversarial review found the unconfirmed version letting a
genuine flash through twelve frames after a pass outlier, which the code before
any of this caught.

Two cheaper answers were tried against measured data first and both lost.
Deferring the resolve by a frame, the way a withheld frame defers it, clears the
run counter in the gap — so the next frame is judged against the dead path with
a fresh run's budget and is withheld in place of the one that was saved: three
frames instead of two, a good frame still held, just a different one. Rebuilding
the path without arming the stand-down took the recorded planet-surface cascade
from two withheld frames to four; the stand-down is not a belt-and-braces
companion to rebuilding the path, it is what covers the rebuild, and in a
cascade the rebuild straddles the alternation and poisons the prediction it is
rebuilding. With the confirmation in place that same replay costs **one** frame,
against **two** for the detector this replaces — holding the fixtures constant, so
that the detector is the only thing varying. Run each tree against its own suite
instead and the before-number is three: the fixtures added here learn separations
and certify parks ahead of the replay, and it inherits them. Both comparisons are
honest and they answer different questions; the two is the one that isolates this
change.

The scope is worth stating, because the numbers invite a wider claim, and the
trade runs in both directions.

It does not catch a rebase with no bad frame in front of it — the 123-unit run
is one of those, and at the moment of decision its two frames are
indistinguishable from a glitch holding still.

And it can cost the two-frame class. The signal is the furthest camera, and this
module's own evidence is that which camera that is moves with the pass mix
rather than with the view: a phantom jump of 4,201 units was measured from pure
pass composition. So two frames at the *same* wrong viewpoint can report
positions thousands of units apart, and this rule would let the second one
through. Set against that, there is no measured example of a genuine two-frame
glitch anywhere — the class rests on one player's description, and the section
below attributes that exact symptom to the external camera, where it happens
identically with the fix switched off. The honest statement is that a measured
fix is being traded against an unmeasured class, in the direction the evidence
points, and that the trade would look different the moment somebody captures
one.

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

The switch survives as `hold_frames_on_external_cam`, defaulting to 0, with that
history written beside it so the idea is neither rediscovered from scratch nor
dismissed on a measurement whose premise has moved.

**If you see a flash that got through:** press **Pause** immediately, then quit
and send `edvr_logs\`. That writes the last ten seconds of viewpoint history,
which shows whether the frame was detected and let through or never detected at
all. Those are different bugs and the log is the only thing that distinguishes
them.
