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

### Round two: the capture came back, and the texture channel is CLOSED

Flown 2026-08-25 (log `edvr_gfx_20260825_103005`). The auto-arm fired on
both zooms — thirty build frames each, q-clocked, with copies, uploads,
resolves and dispatches all visible. The measurements:

- **The body target IS the composite's source.** `res=` identity: the RTV
  the six body draws render into and the SRV both composites sample carry
  one resource pointer. There was never a copy or resolve to find between
  "@79" and "@38" — they were two views of one texture, which the old
  census had no way to say.
- **The body renders complete before either eye reads it.** Per frame, on
  the q clock: six body draws (q=106–111 in the recorded frame), then eye
  A's composite (q=112), then eye B's (q=137). The first capture's "issued
  after both eye blocks" was the same position read without the cycle —
  end of frame N is start of frame N+1.
- **Zero writes to any composite input between the two reads.** The watch
  covered all four sampled resources (the 6x1 strip, the 135x133 and
  542x535 downsample pyramid, the 2170x2142 body) across 60 frames, both
  zooms: nothing. The pyramid rebuild, the strip copies, and both
  per-eye exposure dispatches (`F1FB2EEFB662F3AA`, q=146/157) all land
  after composite B; next frame's composites read them settled.
- **The "tiling in" is not geometry and not body-target writes.** The six
  draws carry identical index counts (36864, 5334×2, 15360, 5334×2) from
  the first captured frame to the last, both zooms. What distinguishes the
  first zoom is a burst of 2,986 copies — texture streaming into the
  families the body render samples. The visible build is the body's INPUT
  textures arriving (plus any shader-side reveal), re-rendered fresh each
  frame — identically for both eyes.
- **EDVR's withholds are pair-symmetric in practice, not just by design.**
  The vr log shows every withheld frame replaced for eye 0 and eye 1
  together, same millisecond, consecutive counters. And notably: the flash
  detector trips ON the FSS zoom itself — the body-render camera sits
  ~9,880 units out and reads as a teleport until "parked" certification
  catches it, costing a few reprojected frames per zoom-in. A judder lead
  worth its own arc, but it cannot split the eyes.

**What is left is the one input the census never recorded: constant-buffer
CONTENTS.** Both composites bind the *same* 208-byte object at VS b0
(`c=@70` — the engine's camera block, bound by body draws and UI draws
alike), which is therefore rewritten between the two eye passes through
Map/Unmap the census cannot see; pixel-stage buffers were not recorded at
all. A scan-reveal progress value that advances per *write* rather than per
*frame* would compose eye B a more-revealed body every frame of the build —
the reported split exactly, through a channel every capture so far was
blind to. This is also the mechanism class this game keeps exhibiting: the
exposure pass runs "once per eye, first eye then second", RemLok draws into
per-eye passes from shared state, and the particle ring showed one buffer
serving many draws through a moving offset.

### The third instrument: the CB watch

`advanced.census_cb_watch = <vh hash>` (2026-08-25). While a census runs,
every recorded draw running that vertex shader dumps `DCW` lines: the
current contents of its VS b0 and PS b0 — refreshed CPU-side from the
Map/Unmap and UpdateSubresource tees, so a dump is exactly the bytes the
GPU reads for that draw — with the draw's q.

### Round three: the constants are clean too, and the game side is CLOSED

Flown 2026-08-25 (log `edvr_gfx_20260825_125743`), 59 frame-pairs of DCW
dumps across two zooms. The composite binds **no pixel-stage b0 at all**;
its 208-byte VS block was rewritten between the two eye passes every frame,
and the diff of eye A's bytes against eye B's says the rewrite is exactly
what it should be:

- f[16..19] — one matrix row plus translation, the ±35-unit eye offset —
  differs every frame: the per-eye placement transform.
- f[39], f[43], f[47] — the same offset expressed in two more spaces.
- A handful of frames differ at 1e-7 in shared fields: float jitter from
  recomputing the same pose, not content.
- **Nothing else, ever.** No reveal field, no progress scalar, nothing
  steps across the build, and the eye order never swaps (first composite
  is the +35 eye in 59 of 59 frames).

With round two this closes the game side end to end: both eyes composite
the same completed body texture through correct per-eye transforms, every
frame, all the way through the build. **The split the headset shows is not
in what the game renders. What no CPU-side capture can see is WHEN the
compositor samples each eye's texture** — Elite reuses one texture per eye
with no fence, the second eye's GPU work is issued last, and under build
load (the streaming burst is exactly that) the compositor can catch eye A
finished and eye B still drawing: one eye persistently a frame ahead,
monocularly real, needing the build and a headset. Every part of the
report, one channel left.

### The candidate fix: snapshot submission

`experimental.submit_snapshot = 1` (2026-08-25, openvr side, live-
reloadable). Every forwarded frame is delivered to the compositor as a
per-eye COPY taken at Submit — the same copies the transition-flash
resubmit already makes every frame, now handed over instead of the live
textures. The two copies are enqueued back to back behind the frame's
rendering, so whatever the compositor's timing, both eyes' delivered
content is latched at the same point: split staleness becomes symmetric
staleness, the pair latch's CONSISTENT-LATE-BEATS-SPLIT rule applied to
pixels. Any refusal (shape change, fault, budget) falls through to the
live texture for that eye-submit, exactly as stock.

**Flown 2026-08-25 (log `edvr_gfx_20260825_133008`): NULL.** A full A/B/A —
ON at startup, OFF at 13:32:30, ON again at 13:33:25, receipts in the vr
log, no shadow faults, and the same copies visibly correct through fifteen
withholds — and the split did not move. The delivery channel is closed with
the other two.

### Where every measurement converges: the resample

What survives all three rounds AND the snapshot null is not a channel that
delivers different content — it is the one legitimate per-eye operation in
the whole pipeline: **each eye resamples the same half-resolution mono
image through its own placement transform** (the ±35 row the DCW dumps
measured). A ring seen near edge-on is a feature a fraction of a pixel
thick in a 2170-wide image; two sample grids that differ by a per-eye
homography catch and miss its pixels DIFFERENTLY — segments present in one
eye and absent in the other. Monocularly real. Worst exactly while the
ring is faint and partial (the streaming build), converging as it
saturates — "one eye still filling while the other is already solid".
Needs the ring (thin), needs the build (low contrast), needs the headset
(two resamples), immune to latching (content identical by construction).
It is also Finding 2 wearing the report's clothes: the pixelated sprite
and the ring split are one under-resolution with two symptoms.

## The fix: `fix.fss_res` — the body layer at full eye resolution

Built 2026-08-25, off by default. Three identity-tracked moves:

1. **CreateTexture2D**: a single-mip, non-MSAA render-target or depth
   texture asked for at exactly eye/2 per axis is created at double the
   requested size. The auto-census's own trigger measured that NOTHING
   else ever draws at that size — it waited 15,654 frames of menus,
   flight and supercruise for the first such draw.
2. **RSSetViewports**: a viewport of exactly the requested half size, set
   while an inflated texture is bound, is scaled ×2 — plus a draw-time
   backstop for a viewport set before the bind. Receipts logged, counted.
3. **The eye test**: the inflated textures are now exactly eye-sized and
   are excluded by identity — the panel-size collision, solved the same
   way.

The composite needs nothing: it samples through normalized UVs and simply
receives a sharper image. Round two's writer inventory (draws only, no
copies, no compute, no resolves into the body target) is what makes the
whole fix this small. Cost: 4× the pixels for one offscreen layer, only
while zoomed. Live — a flip applies at the next zoom, because the textures
are created per zoom.

The A/B doubles as the ring-split test: at full resolution the ring has
four times the pixels, and the per-eye grids agree about a feature two
pixels wide where they disagreed about one. Sharper body either way; if
the split dies with it, the aliasing story is confirmed. If the split
survives full resolution, what remains is the observation procedure — a
sequential monocular check during an evolving build sees different build
moments in each eye by construction, so the decisive re-test is one
deliberate check with the eye order reversed.

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
- `advanced.census_cb_watch = <vh>` — DCW dumps of a watched draw's b0
  constant contents, VS and PS, at the draw, from the Map/Unmap tee
  (2026-08-25).
- The `vScreen declined` totals line, and a note when a **second D3D11 device**
  is created and not hooked (EDVR hooks the first device only).

## Round four: the black squares, and the per-eye tile stack

Flown 2026-08-25 (log `edvr_gfx_20260825_140928`, five captures — four at
stock geometry, one inflated). The field first: `fss_res` is confirmed
sharper and confirmed INNOCENT — the black squares appear at stock
resolution too. And the report sharpened into its final shape: the right
eye's inner-radius-outward sweep is the reveal working as designed; the
left eye showing BLACK 16-pixel squares within the ring AT ALL is the
defect. (Outlined squares animating in both eyes during the
pixelated-to-sharp resolve are symmetric and almost certainly the game's
own scan-tile art.)

What the extended captures measured:

- **Every paired eye draw samples identical resources in ALL EIGHT
  recorded slots** (the x= extension). The per-eye state does not enter
  through sampler identities 0–7.
- **The tile stack exists and is per-eye**: twice per frame, once per eye,
  `9347F8FC2DCE0248` (34,34,16 groups → a 543x536 f60 map) feeds
  `22786F6DE290C577` (272,268 groups → an **eye-sized R16_UINT surface
  written one thread-group per 16x16 tile**) — the black squares' exact
  granularity, refreshed before both composites. Alongside:
  `E65498AE6C2C9F1B` writing THREE 272x268 f62 maps each frame, and two
  shaders (`074CB657FDBD43E6`, `76BFC737F1F8CB83`) writing a 101 MB pool —
  the moving-offset record class this game keeps using.
- The consumer is therefore in a channel still unrecorded: PS slots 8+,
  another shader stage's SRVs, or per-draw offsets into the pool carried
  in constants b1+ (the DCW watch read b0 only).

## Round five: the dispatch probe, and what it taught about probing

`advanced.census_skip_dispatch` (2026-08-25): compute dispatches named by
hash are not forwarded while the spec is set — the draw skips' completing
half. `HASH:N` narrows it to the Nth occurrence per frame (one eye of a
per-eye pair); `experimental.dispatch_pair_sync = HASH[:r]` copies the
first occurrence's UAV0 over the second's, probe and equaliser in one.

The first probe flight produced five nulls and one lesson, and the log
voided the nulls: the session's only zoom preceded every skip window, and
the squares exist only during a build — every skip probed a settled body
with nothing left to change. *A probe against a transient must bracket the
transient: set the spec, THEN trigger the build.* The flight still paid:
four of the five candidates can be skipped for thousands of dispatches
without visibly harming the settled scene, and `E65498AE6C2C9F1B` cannot
be skipped at all — it runs at the top of every frame and the engine
stalls presentation waiting on something derived from it (recovered on
clearing the spec). It is struck from every candidate list.

## Round six: cinema mode, and what the squares turned out to be

The probe redo never needed to fly. Two field observations and two
measurements closed it:

1. **The black squares appear in HMD Cinema Mode** — a mono pipeline, one
   render, both eyes shown the same panel. The squares are born in the
   single render, not in per-eye machinery.
2. **They replay on every re-zoom.** The four back-to-back phase-A
   captures carry identical, flat copy traffic (544/544/540/540 — no
   cold-streaming burst), and the squares showed on each. They are not
   asset arrival.
3. **The streaming moves whole mips, not tiles** — 64 of 544 copies carry
   boxes at all. Sixteen-pixel black squares cannot be missing content
   tiles; sixteen pixels is the granularity of the game's tile
   classification systems and of its scan-effect art.
4. The outlined squares animating in both eyes during the resolve are
   unambiguously styled scan art — the same visual family.

**Conclusion: the black squares are, with high confidence, part of the
game's designed scan-resolve tile animation**, playing in the mono body
layer and composited identically to both eyes.

## Where that leaves the original report

Five instrument rounds measured every objective channel between the game
and the two eyes, and all of them came back symmetric:

- the body texture: one resource, rendered before both composites, zero
  writes between the reads (round two);
- the composite constants: pure per-eye camera placement, no reveal
  field, stable eye order (round three, the DCW dumps);
- the delivery: pair-latched withholds in practice, and a snapshot-
  submission A/B/A that changed nothing (the null that closed it);
- the sampled inputs: identical resources in all eight recorded slots
  (round four);
- the squares themselves: mono-origin, designed (round six).

What remains is the stimulus itself: a rapid, high-contrast, tile-
granular animation playing on a ZERO-DISPARITY mono plane inside a stereo
scene — the hardest case binocular fusion has. Day one's misfusion
explanation died, correctly, for being asserted without evidence; it
survives at the end because five rounds of evidence eliminated everything
else and named the exact stimulus. The percept is real; the pixels are
the same in both eyes.

What EDVR ships from this arc: **`fix.fss_res`** — the body at full eye
resolution, field-confirmed sharper, which softens the whole effect by
giving the animation four times the pixels — plus the instrument suite
(q=, res=, x=, DCC U/V, DCX, DCW, census_auto, census_frames/lines,
census_skip_dispatch with :N, dispatch_pair_sync, submit_snapshot), every
piece of which the next hunt inherits.

## Open

- The flash detector withholds a couple of frames at every fresh FSS zoom
  (the body camera until parked-certification) — a real hitch on exactly
  this transition, and its own arc.
- If the per-eye percept ever needs revisiting: the deliberate monocular
  re-test with eye order reversed, done on a body whose animation is
  mid-flight, is the observation that would reopen it.
- `fix.fss_res` is still opt-in; promoting it to a shipped default is a
  release-train decision (cost: 4x the pixels for one layer, FSS only).
- The flash detector trips on the FSS body camera at every fresh zoom
  (~9,880 units, withheld until "parked" certification) — a hitch on
  exactly the transition being scanned. Its own arc.
- The census still does not record `GenerateMips`,
  `ClearUnorderedAccessView*`, or the contents of command lists
  (`ExecuteCommandList` is seen, its interior is not). The 272x268 tile
  maps are now known to be written by compute (`22786F6DE290C577`, visible
  as DCX) — the 2026-08-24 mystery of "nothing writes them" is resolved by
  the dispatch recording.
- The CB watch reads b0 only. A reveal value living in a later CB slot
  would need the watch widened — one more slot pair, same tee.
