# The menu backdrop

A player reported the background behind Elite's opening menu as "pixelated and
ugly" in the headset. This is what it actually is, how that was established,
and what the fix does and does not do.

## The measurement

The report came with a 1920x1080 screenshot. Three numbers off it settle the
diagnosis before any capture was taken.

**It is very dark.** The maximum value on any channel anywhere in the image is
91 of 255. The whole picture lives in the bottom third of the range, which is
where 8-bit quantization is most visible and where a compressor has the least
to work with.

**It is quantized to the R5G6B5 grid.** Expanding a 5-bit channel to 8 bits
gives steps of about 8; a 6-bit channel gives steps of about 4. Counting how
many pixels land exactly on those grids:

| channel | on the grid | by chance |
|---|---|---|
| red (5-bit) | **79.6%** | ~12.5% |
| green (6-bit) | **79.8%** | ~25% |
| blue (5-bit) | **59.7%** | ~12.5% |

Three to six times chance. R5G6B5 is how BC1 stores the two endpoint colours
of every 4x4 block, and the 20-40% that miss the grid are the one-third and
two-thirds interpolants between them plus bilinear filtering. Nothing else in
a render path produces that histogram.

**It is plateaus, not noise.** Only 12.8% of pixels sit in a run of one
identical pixel. Two thirds are in runs of seven or more, and 15.8% are in
runs of 64 or more. Broad flat patches with visible steps between them.

One negative worth recording: horizontal edge energy was binned by `x mod N`
for every N from 2 to 32, looking for an axis-aligned block grid in screen
space. Every N came back flat -- ratios 1.00 to 1.06, no alignment anywhere.
The blocks are in TEXTURE space, smeared across the screen by a rotated,
magnified mapping. That is what ruled out "a low-resolution buffer displayed
1:1" and pointed at a magnified compressed asset instead.

## What the census found

A draw census taken at the main menu (game build 330683) named it outright.

`@130` is a **1920x1080 BC1_TYPELESS** texture. Across three census frames it
is never a copy destination and never written: a static asset.

**The size is not fixed.** The first field run, minutes later, matched a
**3840x2160** BC1 in the same shape of draw and rebuilt when it later saw the
other one. So there are at least two large BC1 stills behind this menu. Which
of them you are looking at, and whether they are variants of one backdrop or
two separate assets, is not yet established -- the rebuild log line names the
dimensions each time and is what will answer it.

It is drawn by a four-vertex triangle strip, one instance, no depth, stride
24, vertex shader `DEF19B035D5EDEDC` and pixel shader `831DF02EBA8AE814`, into
two 1920x1080 RGBA targets (`@129` and `@141`, the second with a 208-byte
constant buffer). Those two are then sampled by six-index quads through vertex
shader `EF103A7CB4A8369A` into the two eye targets.

```
@130  1920x1080 BC1  (the still, static)
  |   4-vertex trianglestrip, no depth
  +-> @129  1920x1080 RGBA
  +-> @141  1920x1080 RGBA
        |   6-index quad, VS EF103A7CB4A8369A
        +-> the two eye targets
```

Two consequences follow from that chain.

**It is not the vScreen panel.** It never touches the on-foot panel composite,
which is why screen curvature and panel distance have no effect on it -- the
field observation that started the hunt.

**Panel resolution makes it worse.** The backdrop is pinned at 1920x1080 at
every stage before the composite. Raising `vscreen_res_width`/`_height`
sharpens everything around it and magnifies it further, so on a 5120x2880
panel the backdrop is the softest thing in view. This is the opposite of the
intuition and it is worth saying in the ini, which it is.

## The signature

Recognition is by shape and by what the draw reads:

- a non-indexed draw, 4 vertices, 1 instance
- pixel shader slot 0 resolves to a BC1 texture at least 1024x512

In the census that was unique. Exactly one BC1 of that size existed; the other
two were 512x512 and 16x16, and the six sibling draws sharing the same vertex
and pixel shaders all sampled eye-sized targets instead.

Deliberately **not** hash-pinned. A size-and-format signature survives a game
update that merely recompiles shaders, which the FSS panel's transcribed
vertex shaders do not.

## Why a texture is substituted and not a shader

The first plan was a replacement pixel shader for the blit. The census killed
it twice over. `831DF02EBA8AE814` is shared by eight draws in the frame, so it
cannot be swapped by hash without touching seven innocent ones; and replacing
it for one draw would mean reproducing behaviour whose disassembly nobody has.
This project transcribes what the game does rather than assuming it, and there
was nothing to transcribe.

The census also made the shader unnecessary. The source is static, and
something that never changes can be fixed once. So the still is debanded into
an EDVR-owned texture at the first matched draw, and that one draw samples
ours from then on -- `holo_fix`'s substitution exactly, applied to slot 0
instead of slot 1. The game's shader runs untouched.

Three things fall out of that:

- the cost is one dispatch per session, not two per frame;
- the per-eye symmetry problem does not exist, because the blit is a single
  offscreen draw both eyes later sample -- identical content per eye by
  construction rather than by discipline, which is the one thing the FSS arc
  (`fss-scanner.md`, rounds 33-49) proved is worth paying for;
- the dither is baked, so it cannot flicker between frames or between eyes.

## The pass

Five dispatches at growing radius (1, 2, 4, 8, 16 texels), each reading the
previous one's output, ping-ponging between two textures. Each pass averages
the four opposed neighbours where the whole neighbourhood is flat within a
threshold, and keeps the centre pixel where it is not.

The threshold is the entire difference between a deband and a blur. A star, a
hull edge, any real structure exceeds it and passes through untouched; only a
neighbourhood already flat to within a quantization step or two is averaged,
which is precisely where the step between two block endpoints shows as a
contour. Default 10 of 255, tunable 0 to 64. Past about 24 it stops being a
deband and starts eating stars, which is a thing to be able to see rather than
a thing to be protected from.

The last pass adds about one LSB of interleaved-gradient-noise dither, keyed
purely to pixel position, to break the final residual contour.

Everything runs in the stored encoding, not linear. The source is read through
an EDVR-made non-sRGB BC1 view, so the values are what the game's sampler
would have decoded; the result is written back unconverted; and the
game-facing view carries the original view's sRGB-ness. The game's sampler
therefore does exactly what it always did. Working in linear instead would
mean undoing a transfer function on read and reapplying it on write, and a
typed UAV cannot reapply it.

## What it cannot do

No pass restores what BC1 discarded. This reconstructs the ramp between the
steps the compressor left. It does not invent detail, and the still is still a
1920x1080 still magnified across the field of view -- so some softness is the
asset and will remain.

Validated offline against the reported screenshot before any code was written:
the same five-radius pass dropped blue-on-grid clustering from 59.7% to 29.1%
and the blocks visually dissolved, at the cost of some faint stars at the
threshold used for that trial.

## Status

Off by default (`fix.menu_backdrop = stock`) pending field verification.
