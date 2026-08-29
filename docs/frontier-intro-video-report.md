# Intro movie (VR): played head-locked at 1024×576, on a screen the game already knows how to place

**Type:** Bug report — VR rendering
**Area:** Startup / launch idents and front-end movies
**Game version:** 4.4.0.3, build r330683 (long-standing; reproduced on every 2026 build tested)
**Severity:** Cosmetic, but it is the first thing every VR player sees, every session, and it is why most of them press Escape through it.

---

## Summary

Launched in VR, the launch ident is drawn on a **screen-space quad of
1024×576 pixels** — about 27° × 15° of a 103° field of view — pinned to the
player's head. It cannot be looked away from, it does not sit anywhere in the
world, and it is roughly a fifth of the width of the splash screen that
replaces it a moment later.

The splash *is* placed correctly: same vertex shader, same six-index quad,
same 1920×1080 source surface — but with a world-space transform. **The
correct code path is already in the frame, being used by the very next thing
the player sees.** The movie appears to be the one front-end element that
never had its transform updated for VR.

The cut from movie to splash is consequently a jump: the picture changes size
by about 5.5× and moves from the player's face to a screen in the world, in
one frame. Since the movie is evidently authored to lead into the splash,
that transition is the part most visibly lost.

Two lesser issues sit alongside it and are described at the end: the movie
begins several seconds before anything is submitted to the headset, and the
frame in which VR starts takes 2.7 seconds.

## Steps to reproduce

1. Stock installation, no mods, VR enabled.
2. Launch the game and do **not** press Escape.
3. Watch the ident, and move your head while it plays.

**Observed:** the movie is a small rectangle fixed to the centre of view; it
follows head rotation exactly, so it cannot be looked at from any other angle.
When it ends, the splash appears much larger and anchored in the world.

**Expected:** the movie occupies the same virtual screen the splash does, so
the cut between them is continuous.

## Renderer-level characterization

Measured through a D3D11 layer that observes and can substitute individual
draws. No game file, memory or code was modified to obtain any of this.

**The movie's path.** Three `R8_UNORM` planes are uploaded per frame —
1920×1080 luma and two 960×540 chroma, i.e. I420 out of the VP8 DirectShow
decoder. A four-vertex draw converts them to RGB into a **per-eye 1920×1080
surface** (one for each eye, from the same planes). Each eye's surface is
then composited by a six-index quad into the eye texture.

**The composite's placement lives entirely in vertex constant buffer 2.** The
vertex shader is a unit quad scaled and transformed by five `float4`s, and
the pixel shader is a single `sample`. Nothing else in the chain can size or
position the panel.

For the **movie**, `cb2` reads (left eye, one rig, 5424×5356 per eye):

```
cb2[0]  512        288          0   0      half-size, in PIXELS
cb2[1] -0.000368732 0           0   0      = -1/2712, and 2712 = eye width / 2
cb2[2]  0          0.000373413  0   0      = 1/2678,  and 2678 = eye height / 2
cb2[3]  0          0            0   0
cb2[4]  0.193907   0            0   1      w is a CONSTANT 1
```

Two things follow. The panel is 1024×576 **pixels** regardless of headset
resolution. And because `w` never varies there is no perspective divide, so
the quad is painted at fixed screen positions — head-locked by construction
rather than by an anchoring choice.

(`cb2[4].x = 0.193907` is worth noting as a correctness check on the reading:
the headset's published tangents put straight-ahead at NDC 0.193973, so that
term is the asymmetric-frustum centring, and the value is mirrored in the
other eye.)

For the **splash**, moments later, the same shader and the same draw carry a
full view-projection: a 16:9 quad of 8.889 × 5 world units at about 3.35 m,
`w` varying across the quad, and the matrix changing frame to frame with head
motion. It is a screen in the world, with the stereo depth that implies.

**So the two differ by 80 bytes of constants and nothing else.**

## Suggested fix directions

1. **Give the movie's composite the transform the splash already uses.** Same
   shader, same draw, same source surface, same quad — only `cb2` differs.
   This looks like a front-end element that was never revisited when the
   splash was placed in world space, rather than a deliberate choice.
2. **If head-locking is intended**, the size still reads as a bug at 27° in a
   103° field of view, and the pixel-denominated `cb2[0]` means the panel
   shrinks further on every higher-resolution headset. Deriving it from the
   field of view rather than from a pixel count would at least keep it stable
   across hardware.
3. **While in there:** the same 1920×1080 front-end surface and the same
   composite shader serve the loading screens, so anything decided here is
   likely to apply to those too.

## The two adjacent issues

**The movie starts before VR does.** On the measured rig the game creates its
D3D device at +0.9 s and begins drawing the movie at ~3.2 s, but does not ask
for `IVRCompositor` until +4.6 s and submits its first frame at +6.4 s. The
first three to five seconds of a twenty-second ident therefore play to the
desktop window only; in the headset the player is still in the runtime's home
environment. Starting playback after the compositor is up — or bringing the
compositor up earlier — would recover them.

**The handover frame takes 2.7 seconds.** The frame in which the first eye
draw happens measured 2719 ms on one run and over 2 s on every run observed.
It is a single long frame rather than a run of slow ones. Players read it as
the intro freezing.

## Impact note

Every VR player meets this once per session, before anything else. The
common workaround is to skip the intro entirely, which means the launch
idents — including partner and expansion idents — are seen by very few VR
players at all.

The fix is unusually well-bounded: the game already contains the correct
placement for this exact draw, used by the screen that follows it.
