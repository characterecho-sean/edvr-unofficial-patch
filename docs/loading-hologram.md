# The loading screen's shimmering ship

*No Frontier issue number yet, as far as we know — if you file one, this
page is written to be its attachment.*

This is a write-up of what the bug actually does, measured rather than
guessed, and what EDVR does about it from outside the game. It is written
for whoever might fix it properly, inside the game, where it is a one-line
change of texture-coordinate space.

**Short version: the spinning ship on the loading screen is not the mesh it
appears to be — it is a hologram synthesized in screen space from the
model's depth buffer, and its scan pattern is sampled in *screen*
coordinates.** On a monitor that is invisible: the loading camera never
moves, so the pattern just sits on the hull. In a headset your head *is*
the camera, so the pattern stays pinned to your view while the ship stays
pinned to the world — a low-res, UI-orange sheet that slides across the
hull whenever you move, identical in both eyes and therefore at zero stereo
depth inside a hull that has real depth. Focusing on it is genuinely
nauseating, which is how it was found.

---

## The symptom

During loading screens, the spinning ship hologram carries a faint,
ghostly, textured layer inside its silhouette. It looks like a flat,
low-resolution copy of some texture; it is orange like the game's UI; and
it moves with your head instead of with the ship. Most players will only
half-register it as the loading screen looking "swimmy" — deliberately
focusing on it produces the full nausea, because it is a head-locked,
zero-disparity pattern painted onto a world-locked, depth-correct object,
which is a combination the visual system has no good answer to.

## What is actually happening

Everything below was measured through a `d3d11.dll` proxy that can log a
census of the frame's draws and withhold or wrap an individual draw. No
game file, memory or code is touched; the identification was verified in
the field by suppressing single draws and watching what vanished.

**The loading frame is 36 draws, and the ship's mesh is never shown.** The
four large mesh draws (up to ~111k indices each, per eye) render the ship —
but nothing in the frame ever samples their colour output. Only their
**depth** is consumed, through a resolve. The visible hologram is produced
by **one six-index quad per eye** that reads the resolved depth and shades
everywhere the ship is, modulating a **256×256 pattern texture** and
tinting it with the UI palette. Suppressing exactly that quad removes the
entire visible ship, which is what proved the frame's structure: the quad
is not decoration on the model — it *is* the model, as displayed.

**The pattern is sampled in screen space.** That is the bug, and every
observed property follows from it:

- *It moves with your head* — screen-space UVs are head-locked in VR, while
  the depth silhouette they are masked to is world-locked.
- *It is ghostly* — the pattern lands at the same pixel positions in both
  eyes, so it has zero binocular disparity: your eyes place it at the
  screen plane while the hull it covers has real depth.
- *It is low-res* — 256×256 texels stretched across a ~4300-pixel eye.
- *It is orange* — the hologram shader applies the UI tint to everything it
  composites, pattern included.
- *The flat game cannot show it* — with a fixed camera, screen space and
  model space never move relative to each other, so the pattern reads as a
  static material. The bug has existed in plain sight on every monitor, and
  only a moving head can reveal it.

## What EDVR does about it, from outside

`fix.holo_pattern` in `edvr.ini`, live within a second of saving:

- **`steady`** (the default) — for exactly the matched draw, the pattern
  texture in sampler slot 1 is replaced with a uniform 1×1 and the game's
  texture is restored immediately after the draw. The multiplication the
  shader performs becomes an identity, so the hologram keeps its shape,
  depth mask and orange tint, minus the scrolling term. Field-verified: the
  shimmer is gone and the ship renders normally. `advanced.
  holo_pattern_level` tunes the uniform (255 verified as the identity,
  which is also what confirmed the shader multiplies rather than adds).
- **`stock`** — nothing is done.

The draw is recognised by shape — a 6-index instanced quad whose first
sampler resolves to the eye-sized depth texture and whose second is the
256×256 pattern — not by a game version, so an update that changes the
hologram makes this feature do nothing rather than alter a stranger.

## What a fix inside the game would look like

The shader already reconstructs where the ship is from depth; it only needs
to sample its pattern in a space that does not move with the head:

1. **Cheapest: freeze the pattern's reference frame.** Compute the pattern
   UVs from a fixed matrix (the loading scene's original camera) instead of
   the live view. One matrix swap; monitors see no change at all, and in
   VR the pattern stops riding the head.
2. **Better: sample in object or world space.** The quad already has depth;
   reconstructing the world position per pixel and deriving pattern UVs
   from it pins the scan lines to the hull, so they rotate with the ship —
   which is almost certainly what the effect was meant to look like.
3. **While in there:** the same hologram-from-depth technique may be used
   for other holographic elements; any of them sampling patterns in screen
   space has the same VR problem in proportion to its size on screen.

Reproduction for verification is cheap: any loading screen, move your head
side to side and watch the pattern inside the ship's silhouette. In a GPU
capture the composite is the only six-index quad sampling the resolved
depth plus a 256×256 in each eye's frame.
