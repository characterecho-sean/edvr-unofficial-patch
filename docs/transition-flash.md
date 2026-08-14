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
while it is still being drawn and declines to hand it to SteamVR.** SteamVR then
reprojects the previous frame, exactly as it does whenever a game misses a frame,
so the player sees nothing rather than something wrong.

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
  of eleven and twelve withheld frames — and since declining to submit costs
  about 80 ms while the compositor waits for a frame that never comes, that is
  most of a second of judder, which is worse than the flash it was removing.

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

**The discriminator is repetition.** A fixed distance between two render passes
recurs at the same size indefinitely; a real transition does not, because real
motion varies. Of fourteen genuine transitions measured in one session the
closest pair were 5.7% apart, while the cascade pair repeated within 1.5%. So the
first jump of a given size is withheld — it cannot be known to repeat yet — and
matching ones after it are recognised and left alone. `edvr.ini` documents the
tolerance as `transition_flash_repeat_percent`.

The two counts are reported separately in a `transition flash so far:` line,
printed roughly every twenty seconds and only when one of them has moved — so no
such line at all means the fix never fired. It is deliberately not left to the
end of the session: the log has no end. A game's `d3d11.dll` is never unloaded,
only terminated with the process, and the teardown path that runs then is
restricted to what is safe when every other thread is already dead. A summary
written there would be a summary nobody ever saw, which is what the previous one
was for five sessions of debugging.

This matters beyond the judder. Every withheld frame costs the detector its next
frame or two while it rebuilds its prediction, and arrivals and wakes churn the
pass mix for *seconds* — so a cascade flip firing shortly before a genuine bad
frame spent the detector's shot early and the real flash was shown. Some share of
"detected and let through" reports is likely to have been that.

There is still a hard cap of two withholds in a row, and the guard that switches
the whole thing off if it ever starts firing continuously.

**Both eyes of a frame now get the same answer.** The decision is taken once, at
whichever eye the game submits first, and the second eye follows it. Previously
each eye read the flag independently and the flag can legitimately change while a
frame is being drawn, so a change landing between the two submissions would have
shown one eye the current frame and the other the previous one — a one-frame
mismatch between the eyes, which feels like exactly the thing this fix is for.

This is a workaround with a measurable failure mode, kept because the alternative
is watching the flash. It is not a substitute for fixing the ordering.

**If you see a flash that got through:** press **Pause** immediately, then quit
and send `edvr_logs\`. That writes the last ten seconds of viewpoint history,
which shows whether the frame was detected and let through or never detected at
all. Those are different bugs and the log is the only thing that distinguishes
them.
