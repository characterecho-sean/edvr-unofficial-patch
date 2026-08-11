# One eye darker than the other near bright lights

*Frontier issue [47312](https://issues.frontierstore.net/issue-detail/47312)*

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

It copies all four per-eye exposure resources from the first eye to the second,
each frame, immediately after the shader that writes them has run for the second
time. Both eyes then tonemap from one set of parameters.

That is one `CopyResource` per resource per frame and no memory patching — the
game's code is untouched.

Measured with a live toggle, on held poses (within 0.024 m and 3.5°):

| | Background difference between eyes |
|---|---|
| Fix off | **+0.975 stops** |
| Fix on | **−0.170** and **−0.610 stops** |

Press **Scroll Lock** in game to toggle it. Looking at a star with one eye dimmed
and pressing it makes the difference immediate.

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
