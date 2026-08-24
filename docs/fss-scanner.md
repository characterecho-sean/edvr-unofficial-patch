# The Full System Scanner in VR

Investigated 2026-08-24, from a field report: zooming the Full System Scanner
onto a planet **with rings** looks wrong in a headset. The ring "tiles in" over
a few frames and the two eyes appear to disagree about how far that build has
got — one eye still filling while the other is already solid.

Two findings came out of it. Neither is the one the investigation set out to
confirm, and the process of getting there is recorded at the end because it was
expensive and the lessons are reusable.

## Finding 1: the zoomed body is rendered MONO

**In VR, the Full System Scanner renders the zoomed body once, into a single
shared render target, and hands both eyes the same pixels.** There is no
per-eye render of the body at all — no stereo, no disparity, no depth. The
planet you are "zoomed into" is a flat picture pasted in front of you, inside
an otherwise stereo scene.

Measured three independent ways, all from one draw census taken in the scanner
with `advanced.census_offscreen = 1`:

1. **One target, drawn once per frame.** The body is produced by ten draws into
   a single `2170x2142` target, issued *after* both eye blocks, identically in
   every captured frame:

   ```
   EYE @19  (first eye)   1 clear + 10 draws
   EYE @23  (second eye)  1 clear + 10 draws
   HALF-RES @79           10 draws   <- once, not per eye
   ```

2. **Both eyes composite from the same source.** Exactly two draws sample the
   half-res target, one per eye, and their bound resources are identical:

   ```
   DC 0 #36  r=@61 (eye A)  s=@38,@103,@104,@105
   DC 0 #60  r=@74 (eye B)  s=@38,@103,@104,@105
   ```

3. **Suppressing that target removes the body and nothing else.**
   `advanced.census_skip_offscreen = 2170x2142` leaves the scanner's chrome —
   body name, object type, reticle arcs, spectral scale, COMMS — and deletes
   the planet and its rings entirely.

The geometry going into it is real: index counts of 36864, 15360 and 5334.

**Consequence for the reported bug:** the two eyes cannot receive different
body content, because they receive the *same* content. Every fix that tried to
equalise the eyes was equalising something that was already equal. Reproduced
identically under OpenComposite and native SteamVR, so it is not runtime-side
either.

What the player perceives is still real — it was reported repeatedly, on two
bodies, testing one eye at a time. But it is not a difference in pixels. A
flat, zero-disparity image changing rapidly, carrying a thin high-contrast
feature (an edge-on ring), embedded in a stereo scene, is genuinely hard to
fuse, and "one eye is ahead of the other" is a standard way misfusion is
described. That fits every part of the report: it needs a ring, it needs the
build, and it needs a headset.

## Finding 2: the body renders at exactly half eye resolution

`2170x2142` against eye textures of `4340x4284` is **exactly 2.0x down on both
axes**, then upscaled for display. This is the measured cause of the separate
field complaint that FSS bodies "look lower resolution".

Note it is derived from the eye size, not from the panel: an earlier reading in
this investigation attributed the softness to `3408x1917` (which *is* the panel
resolution divided by 1.5023) — that target is the scanner's **chrome** layer,
confirmed by probe, and the attribution was wrong.

The clean 2.0 relationship makes this the same shape of problem
`vscreen_res.cpp` already solves for the on-foot panel: a size chosen in game
code that can be recognised by shape and raised. Not built yet.

## The layers, as measured

| target | contents | confirmed by |
|---|---|---|
| `2170x2142` | **the zoomed body and its rings** | skip removes body, leaves chrome |
| `3408x1917` | scanner framing UI / chrome | skip removes framing UI |
| `4089x3578` | body icon atlas, zoom UI circles | skip removes pre-zoom icons; `vs:E508648660A352B2` removes zoom circles |
| `4340x4284` | the eye textures | published by `openvr_api.dll` |

Shader families that appear **only** inside the scanner (from a diff of a
census taken outside the FSS against one taken inside):

```
26FC402B1274EE7B  X n=36864  -> 2170x2142    9FFA5D5E79F04873  X n=15360  -> 2170x2142
B12F7A618E1BDE98  X n=5334   -> 2170x2142    203DF51758AADC4D  X n=5334   -> 2170x2142
E508648660A352B2  X n=6  x24 -> eye          889A5279E68F0672  X n=13248  -> eye
953C8123AD8DC13B  N n=6       -> eye         B018D143700AB803  X n=6      -> eye
DC937A645B8DB067  N n=6       -> eye
```

## Method notes — four theories died here, and how

Recorded because each died to a *measurement*, and the same traps are latent in
any future hunt on this codebase.

**1. "The eyes sweep the ring in opposite directions."** Died to the cheapest
test available and one that should have been first: closing one eye at a time.
The report changed from "opposite directions" to "one solid, one filling",
which are different bugs. *Ask for the single-eye observation before theorising.*

**2. "The eyes catch a shared texture at different points mid-frame."** Died to
a census: the two eye blocks are identical draw for draw, and the body's draws
are issued after both of them.

**3. "The eyes sample per-eye textures that fill at different rates."** A fix was
built, and its counters were perfect — 10,988 matched draws in each eye, 10,988
snapshots, 10,988 substitutions. It changed nothing. A positive control that
substituted flat **magenta** over those 11,000 draws produced no visible magenta
anywhere, proving the matched draws were invisible. *A counter that climbs is
not evidence that the right object was found. Build the positive control first.*

**4. "A per-eye resolve map is copied out of sync."** `CopyResource` and
`CopySubresourceRegion` were hooked for the census, and the first capture found
two distinct `272x268` R8_UINT textures packed side by side into one `544x268`
buffer and into slices 0/1 of an array — a stereo pair by construction. A fix
substituted the second half's source; 30,575 substitutions, no change. Two
things killed it, both findable beforehand:
- `ceil(4340/16) x ceil(4284/16) = 272 x 268` **exactly**. It is a 16x16 tile
  classification map of the *eye* texture — a screen-space engine fixture, not
  scanner state. Its aspect "matches" the eye because it is *derived* from it.
- The substitution counter climbed to 3,419 **while the game was still in a
  menu**, seventeen seconds before it drew a scene at all.

**The instrument was the bottleneck throughout.** The census recorded only
draws into eye-sized targets, which is why the body — rendered offscreen at
half resolution — was invisible to it for most of a day. Three separate
`census_skip` probes came back "nothing changed" and were read as "wrong
family" when they meant "wrong render path". The decisive test, when it finally
ran, was trivial: `census_skip_range = 1-9999` suppressed **every** eye draw for
six seconds (122,040 draws) and neither the mirror nor the headset changed at
all.

**Two traps worth naming for next time:**
- The **desktop mirror is not the eye textures.** Screenshots of it cannot
  validate an eye-path probe; six probes were run through it before that was
  checked, and their results were void.
- **`foreignContext()` declines silently, and in `CopyVptr` mode it cannot see
  other devices at all** — a second device's contexts keep the runtime's own
  vtable and never reach EDVR's thunks. The declined-draw counter added here
  reports what is refused, but only for contexts that dispatch through our copy.

## Instruments this left behind

- `advanced.census_offscreen` — the census records draws into non-eye targets
  as `DCO` lines. Without this the scanner is invisible.
- `CopyResource` / `CopySubresourceRegion` hooks (slots 47 / 46), census-only —
  copies recorded as `DCC` lines with source, destination, subresource,
  destination x/y and source box.
- `advanced.census_skip_offscreen = WxH` — suppress draws into an offscreen
  target by size. This is what identified every layer in the table above.
- The `vScreen declined` totals line, and a note when a **second D3D11 device**
  is created and not hooked (EDVR hooks the first device only).

## Open

- The resolution fix against `2170x2142` is not built.
- Nothing writes the `272x268` tile maps that any hooked path can see;
  `UpdateSubresource` and `ResolveSubresource` remain unhooked, and the census
  does not record compute dispatches.
- The census still records only the **vertex** shader hash, and constant-buffer
  objects rather than contents. `Map`/`Unmap` are already hooked, so recording
  CB contents is cheap and would have shortened this hunt considerably.
