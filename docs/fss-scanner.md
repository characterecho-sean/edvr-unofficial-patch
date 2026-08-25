# The Full System Scanner in VR

Investigated 2026-08-24, from a field report: zooming the Full System Scanner
onto a planet **with rings** looks wrong in a headset. The ring "tiles in" over
a few frames and the two eyes appear to disagree about how far that build has
got — one eye still filling while the other is already solid.

Two findings came out of the first day. A third section, added 2026-08-25,
reopens the part of the conclusion the field then contradicted, and records
what was built to decide it. The process of the first day is kept at the end
because it was expensive and the lessons are reusable.

## Finding 1: the zoomed body is rendered MONO

**In VR, the Full System Scanner renders the zoomed body once, into a single
shared render target, and hands both eyes the same pixels.** There is no
per-eye render of the body at all — no stereo, no disparity, no depth. The
planet you are "zoomed into" is a flat picture pasted in front of you, inside
an otherwise stereo scene.

Field-confirmed from the other direction on 2026-08-25: at the eyepiece the
zoomed body reads as **a 2D sprite of a projected 3D model**, slightly
pixelated against what the same body looks like approached in the ship. That
is this finding and Finding 2 seen without instruments — flat because mono,
pixelated because half resolution upscaled.

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

Reproduced identically under OpenComposite and native SteamVR, so none of this
is runtime-side.

## Finding 2: the body renders at exactly half eye resolution

`2170x2142` against eye textures of `4340x4284` is **exactly 2.0x down on both
axes**, then upscaled for display. This is the measured cause of the separate
field complaint that FSS bodies "look lower resolution", and of the pixelated
sprite look above.

Note it is derived from the eye size, not from the panel: an earlier reading in
this investigation attributed the softness to `3408x1917` (which *is* the panel
resolution divided by 1.5023) — that target is the scanner's **chrome** layer,
confirmed by probe, and the attribution was wrong.

The clean 2.0 relationship makes this the same shape of problem
`vscreen_res.cpp` already solves for the on-foot panel: a size chosen in game
code that can be recognised by shape and raised. Not built yet, and now
deliberately sequenced AFTER the build capture below: raising the target means
every writer that addresses it in pixel coordinates has to be known first, and
the capture is what produces that inventory.

## The ring split, REOPENED (2026-08-25)

The first day ended by explaining the two-eye report as misfusion: the eyes
receive the same pixels, so a difference between them "cannot" be real, and a
flat zero-disparity image carrying a thin high-contrast edge inside a stereo
scene is genuinely hard to fuse. The field then said no: tested one eye at a
time, **the left and right eye render differently while the ring section is
loading**. A perceptual explanation does not survive a monocular observation.
The conclusion, not the observation, had to give.

Rereading the evidence, the impossibility argument has a hole a measurement
never closed:

**Both eyes sampling the same texture is not both eyes sampling the same
content.** The two composite draws are ~24 draws apart within the frame. If
anything writes the body texture **between** them, eye B reads newer content
than eye A — every frame of the build, converging the moment the build
finishes. That is precisely the reported shape: it needs the build, it needs a
feature where one step of progress is conspicuous (an edge-on ring), and it
needs per-eye inspection to see. A static census diff can never show it,
because by the time anything is steady the eyes agree.

And the ordering evidence was never taken during the build. The "ten draws
after both eye blocks" pattern comes from censuses armed by keypress — which,
with human reaction, means the settled scanner, ~20 frames after the moment
that matters at 90Hz. The build phase, the only frames the bug exists in, was
never captured. Worse, the instrument could not have seen the writers even if
the timing had landed:

- **Copies were recorded on their own counter**, so a copy's position among
  the draws was implicit in log line order and nothing else; nobody read it
  that way.
- **`UpdateSubresource` was unhooked** — the classic CPU path for streamed
  tile uploads, which is what a build that "tiles in" most resembles.
- **`ResolveSubresource` was unhooked** — the only call that turns an MSAA
  render into a sampleable texture. The body is DRAWN into @79 while the
  composites SAMPLE @38, and no recorded event connects them; a resolve is
  the likeliest bridge, and its position in the frame is the whole question.
- **Compute dispatches were not recorded** — the third way a texture changes
  without a draw — even though the exposure fix has always owned the Dispatch
  hook.
- **The census interned views, not resources**, so even asking "is @79 the
  same texture as @38" was unanswerable from a log.

### What EDVR itself is ruled out of

The one EDVR mechanism that could split the eyes — the transition-flash
withhold, which declines Submits per eye — was checked in code and is
latch-protected against exactly this: `SubmitPairLatch` samples the verdict
once per frame at whichever eye submits first and the second eye follows,
reset at WaitGetPoses (frame_flag.h, "CONSISTENT-LATE BEATS SPLIT"). The
resubmit shadow substitutes per-eye copies of the *last forwarded frame*, so
even a substitution/fallback disagreement shows both eyes same-age content.
The split is game-side content, or it is nothing.

### The instruments built to decide it

All census-side, all free when off, shipped in the DLL as of this commit:

- **`q=` on every recorded line** — one shared per-frame ordinal across
  draws (DC/DCO), copies (DCC), and dispatches (DCX). "Did anything write the
  body between the two composites" becomes a grep: find the two composite
  draws' q values, look for DCC/DCX lines whose dst falls between them.
- **`res=` on the interned-id table** — the underlying resource pointer, so
  an SRV and an RTV over the same texture finally show one identity. This is
  what connects (or separates) @79 and @38.
- **DCC kind `U`** — `UpdateSubresource`, with the destination box: a tiled
  CPU upload names its tile.
- **DCC kind `V`** — `ResolveSubresource`, with the format: dst, src, and
  where in the frame it ran.
- **DCX lines** — every compute dispatch while a census runs, with the
  compute shader's content hash (`ch=`) and what UAV slots 0–3 resolve to.
  Recorded through the exposure fix's existing Dispatch hook; no new patch.
- **`advanced.census_frames` / `advanced.census_lines`** — a census can now
  span 30 frames and 16384 lines, because a build outlives three frames and
  a 4096-line cap drops the tail of an offscreen capture exactly where the
  build finishes.
- **`advanced.census_auto = WxH`** — the decisive one. The census arms
  ITSELF on the first draw into a target of that size after two quiet
  seconds, as if the key were pressed at that moment, and records offscreen
  draws regardless of `census_offscreen`. The FSS body target is drawn every
  frame while zoomed and not at all otherwise, so this fires exactly once
  per zoom-in — on the first build frame, which no keypress can catch. At
  most 8 firings a session; re-saving the ini resets the allowance.

Also fixed on the way: `tools/diff_draw_census.py` could not parse a single
census from the current DLL — the emitter grew `off=`/`copies=`/`offscreen=`
fields on 2026-08-24 and the tool's anchored regexes still demanded the old
text, so a field log yielded "0 complete censuses" while the tool's self-test
stayed green on its own synthesized old-format lines. The regexes now
tolerate additive fields and the self-test carries one census in each
vintage.

### The field procedure

One session, one ringed body:

```ini
[advanced]
census_auto = 2170x2142
census_frames = 30
census_lines = 16384
```

(2170x2142 is the body target on the measured headset — eye size / 2. On the
other headset, take any FSS census with `census_offscreen = 1` first and read
the body target's size from the interned table, or just compute eye/2 from
the log's published eye size.)

Then: open the FSS, zoom onto a ringed body, hold still until the ring is
solid. The log should show "census auto: a draw landed in a 2170x2142 target
…" followed by a 30-frame census. Do it twice (leave the FSS, wait a few
seconds, zoom again) for a second capture free. Send the gfx log.

### What the capture decides

Read the two eye-composite draws (the two DC lines whose `s=` includes the
body resource and whose targets are the two eyes) and every DCC/DCX line
whose dst shares their `res=` identity:

- **Writes land between the two composites** → the split is measured, the
  writer path is named, and the fix is mechanical: EDVR snapshots the body
  texture at the first composite of the pair and hands the snapshot to the
  second (one half-res copy per frame, only while the scanner is up — the
  same substitution machinery the panel and magenta probes already use).
  Both eyes then read identical content by construction, whatever the game
  does in between.
- **No writes between them, but the build's draws land differently than the
  settled state** (e.g. the body redraws inside each eye block during the
  build) → same conclusion by another door, same fix shape.
- **Nothing between them and identical build ordering** → the content story
  is genuinely dead, the split is presentation-side after all, and the next
  instrument belongs on the openvr half (per-Submit content hashes). Not
  expected, given the monocular report.

The half-resolution fix (Finding 2) waits on the same capture: the writer
inventory says whether the target can be inflated without breaking a tiled
build (draws scale with a viewport; copies with pixel offsets do not).

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

## Method notes — five theories died here, and how

Recorded because each died to a *measurement*, and the same traps are latent in
any future hunt on this codebase.

**1. "The eyes sweep the ring in opposite directions."** Died to the cheapest
test available and one that should have been first: closing one eye at a time.
The report changed from "opposite directions" to "one solid, one filling",
which are different bugs. *Ask for the single-eye observation before theorising.*

**2. "The eyes catch a shared texture at different points mid-frame."** Died to
a census: the two eye blocks are identical draw for draw, and the body's draws
are issued after both of them. **Half-revived 2026-08-25**: the census that
killed it was of the settled state, and could not see copies-in-order,
uploads, resolves, or compute at all. The revival is narrower than the
original — not "the eyes read mid-write", but "a write lands between the two
composite reads during the build" — and it is now instrumented to live or die
on a q=-ordered capture rather than an argument.

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

**5. "The difference is misfusion, not pixels."** Died in the field to the
monocular report above, after shipping in the first version of this document
as a conclusion. It was the only theory of the five that was never given a
falsifiable test before being written down. *An explanation that makes the
observer the mechanism needs the same standard of evidence as one that makes
the renderer the mechanism.*

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
- `UpdateSubresource` / `ResolveSubresource` hooks (slots 48 / 57),
  census-only — `DCC` kinds `U` and `V` (2026-08-25).
- `DCX` dispatch lines through the exposure fix's Dispatch hook, with compute
  shader hash and UAV resolutions (2026-08-25).
- The shared `q=` event ordinal and the `res=` resource identity column
  (2026-08-25).
- `advanced.census_skip_offscreen = WxH` — suppress draws into an offscreen
  target by size. This is what identified every layer in the table above.
- `advanced.census_frames`, `advanced.census_lines`,
  `advanced.census_auto = WxH` — long censuses, and the self-arming capture
  for builds too brief to catch by hand (2026-08-25).
- The `vScreen declined` totals line, and a note when a **second D3D11 device**
  is created and not hooked (EDVR hooks the first device only).

## Open

- The build-phase capture itself: fly the field procedure above and read the
  q= interleave. Everything downstream — the equalizer fix, the resolution
  fix — is sequenced behind it.
- The resolution fix against `2170x2142` is not built.
- Nothing writes the `272x268` tile maps that any hooked path can see; the
  census still does not record `GenerateMips`, `ClearUnorderedAccessView*`,
  or the contents of command lists (`ExecuteCommandList` is seen, its
  interior is not).
- The census still records constant-buffer objects rather than contents.
  `Map`/`Unmap` are already hooked, so recording CB contents is cheap and
  would have shortened this hunt considerably.
