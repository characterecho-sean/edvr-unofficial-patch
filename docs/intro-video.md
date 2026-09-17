# The intro movie

## Status

*Written 2026-09-15 from the entries dated 2026-08-28, 2026-09-13 (Flights
09:43 and 10:09; fbd8284, 2aa0c2d, 0d7251b) and 2026-09-15 (two entries).
Restates the journal below; update it whenever this doc changes.*

- **State:** (1) `fix.intro_video` defaults to `screen` (world-lock + FSR
  on the splash's screen), shipped since 2026-08-28 (f908be2); `skip` is
  the off-by-default alternative, SHIPPED v0.16.2, ~6 s saved not ~20 s.
  (2) The native OpenXR runtime builds its whole session once inside
  `VR_InitInternal`, no OpenComposite-style rebuild at the first
  compositor call: the removed `fix.vr_handover = early` has no native
  analogue -- the movie already plays natively on the splash's screen,
  world-locked (2026-09-15 18:30 Steam flight). (3) NGX warm-up
  (`advanced.temporal_aa_warm`, default on): BUILT 2026-09-15, NOT
  FLOWN -- NVIDIA's one-time initialisation and both eyes' features are
  made on a loading frame instead of inside the first Submit (857 ms in
  that flight). Of that, eye 0's create is <= 58 ms; the ~760 ms before
  it is UNSPLIT between NGX init, five 16-MP history textures and the
  runtime UI-resolve compile -- the new per-stage ms lines split it
  (first 2026-09-15 entry). (4) 2026-09-17: a SKIPPED intro left the
  panel and resample armed all session, and the on-foot HUD passed for
  the movie -- the composite sampled the 8192x4608 resample instead of
  the panel, which killed `fix.panel_distance`/curvature on foot and cost
  three passes a frame. FIXED by retiring at the first rendered scene
  unconditionally (last entry); built green, NOT FLOWN; flight brief there.
- **Open:** whether the movie reaches the headset earlier once the
  warm-up flies, or the game just absorbs the stall (section 9, first
  2026-09-15 entry); ident-open -> first-composite latency, never
  measured natively; why the movie was visible only ~2 s in the 18:30
  flight -- keypress or otherwise, unknown. (5) 2026-09-17: facing the
  wrong way after the cut -- MEASURED from ten flights' own logs, root
  cause found (the one-shot seated-origin recentre at `VR_InitInternal`
  start has no settle window and fires on the first tracked pose, whatever
  direction that is); Sean's call was to recentre once the movie plays or
  the splash is visible instead -- BUILT (14ec395's follow-up), NOT
  FLOWN (last entry).
- **Ruled out:**
  - `fix.panel_distance` moving the panel: the draw binds no constant buffer.
  - `fix.black_void` blackening the surround: a full-eye blit, not a clear.
  - Placement "in four vertices": refuted by flight 3; it is VS `cb2`.
  - A viewport counter-move to world-lock the panel: translates, no stereo.
  - Hooking only the exe's file imports to skip: quartz.dll's reader bypassed it.
  - The resample cache keyed on the source: each eye destroyed the other's build.
  - The "DRAWING anyway" line after Flight 10:09's WORKED verdict: the
    front end's own loop on the same composite, not the ident (0d7251b).
  - The early handover's mechanism natively: the session forms once
    inside `VR_InitInternal` (native_runtime_host.h:424-467); the first
    compositor call costs ~7 ms.
  - Phase A = 6.4 s as a native number: that was an OpenComposite
    reading; natively the first frame reaches the headset ~5.0 s after
    the device.
  - The on-foot panel going flat and far as a vscreen regression: the
    intro panel's own false match (last entry).
- **Next flight:** Steam, `fix.intro_video = screen`,
  `advanced.intro_probe = 1`, reading `intro probe: watching the movie's
  open`, `intro probe: the game opened <file> at +X.XXX s after the
  device`, and `intro video lock: holding` against the device line --
  plus the first 2026-09-15 entry's section 9 for the warm-up.
- **Environment:** The placement fix was measured on a Frontier
  install, game build 330683, Pimax via OpenComposite, eye 5424x5356;
  placement is VS cb2 (80 bytes). The skip's flights are Steam, no
  headset needed. Both 2026-09-15 flights are Steam, Pimax Crystal
  Super on Pimax OpenXR, eye 4068x4016 then 2644x2610 once HMD Quality
  applies; the warm-up runs only on the native OpenXR path.
- **Detail:** Placement is "Flight 3" through "Flight 6", "Stage one,
  built" and "What shipped"; "What the bugs were" is process lessons;
  the skip is "Not playing it at all", "Flight 09:43" and "Flight
  10:09"; the warm-up is the first 2026-09-15 entry; the native-runtime
  correction is the second. Companion: docs/loading-panel-handoff.md,
  docs/loading-scrim.md.

Reported 2026-08-28 and **measured the same day**, across two flights, on the
field rig. The page opened with a hypothesis; flight 1 confirmed it, flight 3
refuted where the fix would go. Both are below: the correction is the finding.

## The defect, in the field's words

> When the game first loads in VR, it attempts to play a video at mirrored
> window full resolution, but freezes for 1 or 2 seconds before continuing
> the video in a small, headtrack locked video.

And, added after the flight, the observation that settles the first phase:

> the game loads the video before I am dropped into the game world in VR
> from the Pimax home space

Wanted instead:

> load correctly into a black void (the same one that shows up later after
> the video plays or is skipped) and render the video correctly to a panel in
> 3d space (like the splash screen it already renders in the void)

Most players press Escape through it, which is why this was never reported as
a bug: it is skipped rather than endured.

**And the constraint that decides the whole design**, from the field after
the flight:

> the video is supposed to tie into the splash screen that gets displayed
> after it ends so it would be great if we played in the same virtual panel
> the splash occupies

This is not a comfort preference to be tuned by feel. The movie's last frame
is meant to become the splash: if the two are drawn at different sizes or
distances, the cut is broken however pleasant each one looks alone. **The
target placement is therefore not a number anybody chooses — it is whatever
the splash's own quad already uses.**

That is reachable, because the two are the same draw. `menu-backdrop.md`
measured the menu still arriving in the eyes as six-index quads through
**`vh=EF103A7CB4A8369A`** from a 1920x1080 surface, which is the signature
flight 1 found for the movie panel, exactly. Same shader, same source size,
same shape of draw — and flight 3 then showed that shared draw covers the
whole eye in both cases, so what has to be matched is not where the panel is
but **how much of the 1920x1080 surface each one fills**. Measured for both,
never invented for either.

## The sequence after the movie (field, 2026-08-28)

There is no pause to work with, which every plan on this page had assumed
there was:

> There's no menu to sit at, it automatically drops into the splash, then
> some loading modals and then the main menu

So the states run **movie → splash → loading modals → main menu**, with the
splash lasting a few seconds before the modals cover it. That is what "the
same virtual panel the splash occupies" names: the screen immediately after
the movie, not the main menu behind everything later.

Two consequences. A capture aimed at "the splash" has a window of seconds
and cannot be arranged by asking the player to hold still somewhere; it has
to be placed on the clock. And a capture that lands late measures the MAIN
MENU's composite, which is only the same thing if the game does not rewrite
the composite between those states -- which is exactly what is being
measured, so it cannot be assumed on the way in.

The loading modals in that window are the other workstream's subject
(`docs/loading-panel-handoff.md`, whose scope note reads "the intro only").
Both fixes live in the same few seconds of the startup and neither should
be read from a log without the other in mind.

## What the movie is (measured, from the install)

`Products\elite-dangerous-odyssey-64\Movies\` holds ten Matroska/WebM files.
Every one is **1920x1080**; the launch idents run about twenty seconds.

| file | codec | pixels | length |
|---|---|---|---|
| `Ident_Frontier_EliteNeutral.webm` | VP8 | 1920x1080 | 20.3 s |
| `Ident_Frontier_EliteHorizons.webm` | VP8 | 1920x1080 | 20.5 s |
| `Ident_Frontier_Arena.webm` | VP8 | 1920x1080 | 18.5 s |
| `intro_temp.webm` | VP9 | 1920x1080 | 20.3 s |
| `FrontEnd0/1/A.webm` | VP8 | 1920x1080 | 90-95 s |
| Salvation, Thargoid, Update 14 | VP8 | 1920x1080 | 63-199 s |

They are decoded by the DirectShow filters shipped beside the executable:
`webmsplit64.dll`, `vp8decoder64.dll`, `vp9decoder64.dll`, and
`dsfVorbisDecoder64.dll` for the audio. The frames therefore arrive on the
CPU and reach the GPU as an upload, never as a draw's output — which is why
the census finds them as *source textures* and nothing else.

## The path the movie takes (measured, census 3, t = 12 s)

Four draws a frame carry it, and the census names all four:

```
DCO  N n=4  r=@120 (1920x1080)  s=@121,@122,@123  vh=20F383BBAC05C031  ph=67A1C6A38826030A
DCO  N n=4  r=@134 (1920x1080)  s=@121,@122,@123  vh=20F383BBAC05C031  ph=67A1C6A38826030A
DC   X n=6  r=@82  (5424x5356)  s=@125 (= @120)   vh=EF103A7CB4A8369A  ph=DED8796049C7BB4A
DC   X n=6  r=@118 (5424x5356)  s=@135 (= @134)   vh=EF103A7CB4A8369A  ph=DED8796049C7BB4A
```

**The three planes are the movie.** `@121` is 1920x1080 at format 60
(`R8_UNORM`); `@122` and `@123` are 960x540 at the same format. Full-size
luma, two half-size chroma planes: I420 / YUV 4:2:0, exactly what a VP8
decoder emits. `ph=67A1C6A38826030A` is the YUV-to-RGB shader.

**The conversion runs twice, into two separate 1920x1080 surfaces, one per
eye**, from the same three planes. The two eyes' movie content is therefore
identical by construction — there is no stereo in it to break.

**The panel is a 6-index quad, `vh=EF103A7CB4A8369A`** — the same vertex
shader `menu-backdrop.md` measured for the front end's composite, which is
what predicted this draw before the flight. One per eye, into the eye
texture directly.

It draws from its **own 80-byte vertex buffer** (stride 20, four vertices —
one buffer per eye, not the shared 4 MB widget pool the loader arc fought),
and it binds **no constant buffer** (census `c=-`).

> This page said next, for a day: *"its placement is in four vertices and
> nowhere else"*. **Flight 3 refuted that** — see below. The four vertices
> turned out to be a full-screen quad with nothing in it to move, and the
> paragraph is kept because the correction is the finding.

The rest of each eye's frame, in order: a full-eye blit from another
5424x5356 texture (`vh=20F383BBAC05C031`) — this is the surround, and it is
a **draw, not a clear** — then the movie panel, then one world-quad
(`vh=A888D51024D9798E`, with depth, a 512x512 BC1 and a 532x317). Twenty
eye draws a frame in total, thirty in the frame.

The desktop window is also fed from here: `vh=01C3B84C82172B56` blits both
eye textures into the 2560x1440 swapchain.

## The startup, phase by phase (measured, `intro_probe`)

Times are from the first frame edge; `*` marks an eye-sized target.

| t | what |
|---|---|
| 0.00 s | frame 0 — **0 draws** |
| 1.20 s | frame 1 — **stalls 1203 ms** |
| 1.2 → 3.16 s | ~2224 frames, still **zero draws**, about 1130 fps of nothing |
| 3.16 s | first draws: `512x512 x1, 8x8 x1` |
| 3.25 s | `2560x1440 x6, 640x360 x1, 320x180 x6` — the desktop window only |
| 3.69 s | **stalls 281 ms**, then steady at `2560x1440 x10, 320x180 x3` |
| **6.41 s** | frame 2357: **stalls 2719 ms**, and is the **first frame with a draw into an eye texture** |
| 6.41 s + | steady: `5424x5356* x20, 2560x1440 x2, 1920x1080 x2, 678x669 x6` |
| ~27.8 s | `4259x2395` joins — the loader's dialogs; the movie is over |

**The freeze is one frame of 2719 ms, and it is the VR handover itself.** The
compositor's first Submit lands about 13 ms after that frame's draws
(openvr log, 13:44:49.654 against 13:44:49.638). The field's "1 or 2
seconds" is 2.7.

**Phase A never reaches the headset.** No eye-texture draw and no Submit
before 6.41 s: for the first six and a half seconds Elite hands the
compositor nothing, and the headset is showing the runtime's own scene. The
field saw exactly this and said so — *Pimax home space, until I am dropped
in*. Elite is rendering to its 2560x1440 window all the while, which is the
"mirrored window full resolution" of the report.

The consequence for a fix is hard and worth stating plainly: **EDVR cannot
put a black void in phase A.** There is no Elite frame to modify, and the
frame the headset *is* showing belongs to the runtime.

## Two hypotheses this flight killed

**`fix.panel_distance` cannot move this panel.** Before the flight the panel
size and the front-end surface size coinciding at 1920x1080 looked like it
might already work. It cannot: that fix substitutes the composite's
transform constants, and this draw binds no constant buffer at all.

**`fix.black_void` was never going to reach this.** The surround is a
full-eye *blit*, not a clear. Making it black is a question about what that
blit's source texture holds, not about a clear colour — and the clears the
probe did record (an eye-sized target to black at a=0, and another to opaque
white) are not it.

## Flight 3, and the model it refuted

`quad_probe = 5424x5356:X:6:120`, captured two seconds into steady playback:

```
occurrence 0: baseVertex 0, startIndex 0, viewport 0,0 5424x5356, scissor off
  quad 0: x -1.000..1.000  y -1.000..1.000  (w 2.000 h 2.000)
          +00000000 00000000 0000803F
occurrence 2: identical
```

(Occurrences 1 and 3 are the stride-8 `A888D51024D9798E` draws; the probe
could not read them, for reasons in the next section.)

**The movie's composite is a full-screen quad in clip space.** ±1 on both
axes, uv (0,0), a trailing 1.0f, viewport the whole eye, scissor off. There
is no rectangle in it to reposition, and no fix can move it by rewriting
those vertices, because they do not describe a placement — they describe
"all of it".

Two things follow, and the second is the useful one.

**The head-locking is not a bug to be found, it is what a full-screen
composite IS.** A quad at fixed NDC coordinates is painted at fixed screen
positions in each eye, so it cannot move with the world by construction. No
amount of looking for a transform will turn one up.

**The small picture is made upstream, inside the 1920x1080 surface.** The
composite blits that whole surface over the whole eye; if the picture reads
as a small rectangle in a black field, then the surface itself is mostly
black with the movie in part of it. The intro probe already recorded the
supporting half — *a 1920x1080 target cleared to r=0 g=0 b=0 a=0* — and the
YUV-to-RGB draw that follows it is a four-vertex `N`. Whether that draw
fills the surface or a sub-rect of it, by viewport or by its own vertices,
is now the whole question. **It is one line of a capture, and it was
unaskable until this flight, because the probe refused four-vertex draws.**

If that is the mechanism, the design constraint gets easier rather than
harder: movie and splash already share one panel — the full-screen composite
— and the difference between them is only how much of the 1920x1080 surface
each one fills. That is an inner rectangle to match, not a placement in
space to invent.

## Flight 4: the fill is full-screen too, and that explains nothing

`quad_probe = 1920x1080:N:4:1200`, the draw that fills the composite's
source:

```
quad probe: 2 occurrence(s) of N:4 into 1920x1080, over 1 distinct vertex buffer(s)
  occurrence 0: startVertex 0, stride 16, buffer 0, viewport 0,0 1920x1080, scissor off
    quad 0: x -1.000..1.000  y -1.000..1.000  (w 2.000 h 2.000)  +0000803F00000000
  occurrence 1: identical
```

Stride 16 and one shared vertex buffer identify it as the YUV-to-RGB fill
for both eyes -- and the *one* buffer is itself a measurement, since the
composites carry one each; the per-buffer capture built after flight 3 is
what can tell those two cases apart at all.

**It covers the whole surface.** Full viewport, vertices ±1, no scissor. No
sub-rect, no letterbox, no viewport trick.

So both measured stages are full-screen: the fill covers the whole
1920x1080, and the composite covers the whole eye. A 16:9 movie blitted
across a near-square eye should be stretched and enormous. **It is neither**,
which the field's screenshot settles: a small 16:9 picture, correct aspect,
in a field of pure black, and -- confirmed in the headset -- centred, and
staying centred as the head moves. (The mirror window shows it off-centre;
that is the mirror fitting a near-square eye texture into a 16:9 window, not
what the player sees.)

Two things follow. **The black void half of the request already holds**
during the movie: the surround is black, measured and seen. And **the size
comes from somewhere neither capture has read.**

The one number never taken is the **uv range**. The probe reported the
*position* extent across a quad's four vertices but only the *first*
vertex's trailing bytes -- so for both draws exactly one corner's texture
coordinate is known and the span is not. If the composite samples its
1920x1080 source with uvs reaching outside 0..1 against a black border, the
picture shrinks inside the eye and sits in black, which is what the
screenshot shows and is the natural way to fit 16:9 into a 103°x103° view
without distorting it. That is a hypothesis with a hole in it until the span
is read; the probe now reports it (`uv A..B, C..D (span W x H)`).

## Flight 5, and the answer: the panel is placed by VS cb2

The uv span came back as `0.0000..0.0000, 0.0000..1.0000` -- a u that does
not vary, which no working blit can have. The reading was wrong, and the way
it was wrong is the finding: **the probe was told a layout it had guessed.**
The composite's own input signature, from its disassembly
(`docs/shaders/intro-composite-vs.asm`):

```
// POSITION   0   xyz    register 0
// TEXCOORD   0   xy     register 1
```

`POSITION` is **xyz**, so the 20-byte vertex is position at offset 0..11 and
texcoord at **12**, not 8. The pair the probe printed as "uv" was
`(position.z, texcoord.x)`: z is 0 on every vertex, u runs 0..1. A plain
unit quad with a plain 0..1 blit, misread through offsets nobody had
checked against the shader. The probe now labels those columns by byte
offset alone (`+8`, `+12`, `+16`) and leaves the meaning to whoever has the
signature in front of them.

**And the shader says where the placement lives.** The whole vertex program:

```
mov  o0.xy, v1.xyxx                    ; texcoord straight through
mul  r0.xy, v0.xyxx, cb2[0].xyxx       ; SCALE the unit quad
mul  r1.xyzw, r0.yyyy, cb2[2].xyzw
mad  r0.xyzw, r0.xxxx, cb2[1].xyzw, r1.xyzw
mad  r0.xyzw, v0.zzzz, cb2[3].xyzw, r0.xyzw
add  o1.xyzw, r0.xyzw, cb2[4].xyzw     ; TRANSLATE
```

and the pixel shader (`docs/shaders/intro-composite-ps.asm`) is one
instruction -- `sample(t0, uv)` -- with no fit, no letterbox, no border
term.

So the chain is: a unit quad, scaled by `cb2[0].xy`, put through the matrix
in `cb2[1..3]`, translated by `cb2[4]`, and sampled 1:1. **Nothing else in
it can make the picture small or put it anywhere. The panel's size and
position are entirely in the vertex shader's constant buffer 2**, and every
"full-screen, nothing to move" reading on this page was true of the
vertices and blind to the transform that scales them.

That also retires the uv hypothesis and the sampler-border story with it.

**Why four flights missed it.** The census's `c=` column reads **b0**, and
so does `census_cb_watch`. This draw's transform is in **b2**. The census
therefore reported `c=-` -- no constant buffer -- for a draw whose entire
behaviour is one, and every model built on that line inherited the error.
`docs/loading-scrim.md` had already written the same sentence about the same
blind spot for a different shader: *"the DCW instrument dumps b0, and this
shader reads b2, so the tint has not been measured."* It was a known gap,
recorded, and not closed until it cost a second workstream.
`advanced.census_cb_slot` closes it.

## Flight 6: the right buffers, unreadable — and the splash's real window

The watch found exactly what the shader predicted:

```
DCW register v cb=@127 bytes=80
DCW register v cb=@138 bytes=80
```

**Two 80-byte vertex constant buffers, one per eye** — and 80 bytes is
precisely `cb2[0..4]`, the five float4s the disassembly declares. The
instrument is pointed at the right thing.

Every dump then said **`unwritten`**. Both write tees are wired — Map/Unmap
and UpdateSubresource, checked — so the conclusion is not that the census
missed a write but that **there was no write to miss**: the game fills these
buffers before any census can arm and reuses them. A write tee is the wrong
shape of instrument for a buffer that is written once.

So the watch gained a direct read: when a dump would say "unwritten", the
buffer is copied on the GPU and read back four frames later as a `DCW read`
line. Copy-settle-map, the shape `panel_quad` and `quad_probe` already use.
The write tee stays primary because it is per-DRAW exact and a copy is not;
the read is the fallback, and its line says which it is.

**And the second census missed the splash entirely.** It asked for 40 s and
landed at 40 s — into a *rendered scene*, 364 draws and 483 offscreen, where
the composite never runs. This session's own markers say why:

| t | marker |
|---|---|
| 12.0 s | census 1 — mid-movie, composite present, buffers found |
| 24.2 s | `menu backdrop: SMOOTH` — the first still appears: **the splash arrives** |
| 28.3 s | `loading dim: OFF engaged` — the loader modals start |
| 28.9 s | `loading panel: FIT -- measurement 1` — modals up |
| **40.5 s** | `loading panel: a rendered scene arrived -- the intro is over` |
| 40.0 s | census 2 — landed on that boundary, in the main menu |

The splash window is **24 s to 40 s**, sixteen seconds wide, and 40 was the
one moment in it that does not work. The loader panel's own scene-boundary
line is an independent marker for the end of it, which is worth remembering:
the two workstreams are measuring the same stretch and can date each other's
captures.

## Stage one, built: fix.intro_video_size

`src/d3d11/intro_panel.cpp`. Multiplies `cb2[0].xy` for the movie's two
composite draws, so the panel grows about the point it already sits on --
straight ahead, both eyes, aspect kept. 1.0 is stock and off; 4.0 is the
ceiling. It does NOT move the panel into world space, so it stays
head-locked and the cut to the splash still will not line up. That is
stage two.

Three things keep it off everything else:

* the draw must be the six-index composite **sampling the surface the
  movie was converted into in the same frame** -- the YUV fill draws
  before it, so its marker is fresh;
* the constants must READ as a screen-space placement -- `cb2[3]` all
  zeros and `cb2[4].w` exactly 1. Anything world-placed, the splash and
  the menu included, refuses by its own numbers, and the refusal stands
  the fix down for the session with the numbers in the log;
* the first rendered scene retires it, the scope rule `loader_panel.h`
  already states.

The constants are read by GPU copy, once per eye, not from the write tee
-- valid because they are static, which flight 6 measured rather than
assumed.

## The other symptom: facing the wrong way at the cut

Reported after the flight:

> when the movie ends, I'm looking the wrong direction, I have to hit the
> recenter HMD display key a few times to get centered onto the splash screen

This is consistent with the freeze and probably the same root cause. The
movie is head-locked by construction (above), so it looks correct from any
angle and reveals nothing; the splash is the first thing anchored to the
game's world, so it is the first thing that can be in the wrong place. If
the game establishes its forward direction while bringing VR up — which is
the 2719 ms frame at 6.41 s, when the player is still in the runtime's home
space with their head wherever it happens to be — then the splash is
anchored to a direction nobody was facing.

**Unmeasured.** The openvr half already records the headset pose every frame
before any EDVR offset, so the pose side is available; what is not yet read
is what the game asks the runtime for around the handover. Worth doing
before anything is built, because if this is what it looks like, it is a
second fix and not part of the panel one.

## What the instruments learned about themselves

**A census cannot be armed inside a stall.** `census_at_ms = 2000,5000,12000`
fired at 2078 ms, **6407 ms** and 12000 ms. The middle one asked for 5 s and
landed at 6.4 — the 2719 ms freeze contains no frame edges to arm on, so an
entry that comes due inside one fires when the stall ends. Aiming inside
phase A means asking for a time comfortably before it: 4000 ms lands there.

**The composition digest records targets, not sources.** It is blind to the
YUV planes wherever they are not being drawn into. So it cannot say whether
the movie is already playing during phase A — see the open question below.

**`quad_probe` could not see this draw at all** until 2026-08-28. It was
offered only draws that missed the eye textures, because everything it had
been aimed at is built in an interface surface. A spec naming the eye's own
size matched nothing, silently. It is now offered every draw
(`vscreen.cpp`, above the eye gate).

**`quad_probe` copied ONE vertex buffer per capture**, taken at the first
occurrence, and decoded every later occurrence out of it whatever buffer
that occurrence had actually bound. Right for the widget panels it began
with — they share a single 4 MB pool — and silently wrong here, where the
two eyes' composites carry one 80-byte buffer each: occurrence 2 was decoded
from occurrence 0's bytes and reported geometry identical to it **by
construction**. An instrument agreeing with itself reads exactly like a
measurement, which is this project's recurring way of losing a week. It now
keys a staging copy per distinct buffer (up to eight), and an occurrence
whose buffer did not fit says so instead of borrowing one.

The same flight explains occurrences 1 and 3's *"an index landed outside the
copied range"*: those draws bind the 32 KB stride-8 buffer, which was never
copied at all, and were being read at the wrong stride into the wrong
buffer. They now decode correctly or say why not.

**`intro_probe`'s per-frame times are quantized to nothing.** `nowMs()` is
`GetTickCount64`, resolution about 15.6 ms, which is right for the 2719 ms
freeze it was built to measure (0.6% error) and useless for a frame delta:
at these rates it can only ever print 0 or ~16. The startup phase actually
runs at **178 fps**, measured by frame COUNT over a twenty-second window.
Its "16 ms" lines were read as 62 fps when flight 4's skip was chosen, and
that is why the capture landed nineteen seconds early, mid-movie, instead of
on the splash. The freeze numbers stand; the per-frame ones must not be read
as frame times, and the page says so where they appear.

**A frame count cannot express "after the movie" at all.** The rate swings
from 178 fps during playback to about 13 at the menu, so any skip large
enough to clear a twenty-second movie leaves the player waiting minutes at
the menu for the remainder to trickle past. `common/timing.h` states this
rule -- if it answers "how long", it is milliseconds -- and `census_at_ms`
had already learned it. `advanced.quad_probe_at_ms` is the same lesson
applied here, composed with the skip so both must be satisfied.

**And it refused four-vertex draws outright** — `COUNT` had to be a multiple
of six. That rule was about interpreting indices as quads and had no
business applying to the non-indexed kinds, where a four-vertex triangle
strip is exactly one quad. It is the shape the YUV-to-RGB draw uses, so the
one measurement now wanted was the one measurement the probe could not be
asked for. `D` and `N` now take `COUNT` 4, and the caller passes the draw's
start vertex where an indexed draw passes baseVertex.

## What is still unmeasured

1. **Where the picture sits inside the 1920x1080 surface**, and by what —
   the viewport of the YUV-to-RGB draw, or its own four vertices. This
   replaces "the four vertices of the composite", which flight 3 answered
   and which turned out to be full-screen. It is the only number a placement
   fix needs, and `quad_probe` can now be aimed at it (`1920x1080:N:4`).
2. **The same number for the splash**, which flight 3 was meant to capture
   and did not — the session ended before the probe could be re-armed at the
   menu. Without it there is nothing to match the movie *to*.
3. **Whether the game's forward direction is latched during the handover**,
   which is what facing the wrong way at the cut looks like from outside.
4. **Whether the movie is already playing in phase A.** The ten draws a
   frame into 2560x1440 between 3.7 s and 6.4 s are unidentified; the digest
   cannot see source textures, so the YUV planes may or may not be in them.
   One census at 4000 ms settles it. It changes nothing about the fix — phase
   A is unreachable either way — but it decides whether the report's "plays,
   then freezes, then continues" is one continuous movie or two things.
3. **What the surround blit's source holds.** If it is already black, the
   "black void" half of the request is satisfied from the handover onward and
   only the panel needs work.
4. **Whether EDVR is party to either stall.** Never A/B'd. Renaming
   `d3d11.dll` aside for one launch settles it and costs nothing.

## The flight plan from here

**Flight 6 — the transform, in both states.** The mechanism is settled; what
is left is two sets of numbers.

```ini
[advanced]
census_cb_watch = EF103A7CB4A8369A
census_cb_slot  = 2
census_at_ms    = 12000,28000,34000
census_frames   = 2
quad_probe      =
```

Two censuses: one mid-movie, one after the splash has arrived. Each dumps
the composite's `cb2` for both eyes, per draw, as `DCW` lines. `cb2[0].xy`
is the scale and `cb2[4]` the translation, so the two dumps say outright
whether the game moves this panel between the movie and the splash, and by
how much.

The times come from the measured startup rather than from an estimate. The
handover lands between 6.4 s and 8.6 s depending on the run; the movie is
about twenty seconds; the splash then holds from about 24 s until the
rendered menu arrives at about 40 s. 12 s is comfortably inside playback,
and 28 and 34 both sit inside the splash window with room on either side --
two shots at it, because the boundaries move by a second or two between
runs and flight 6 lost its second census by landing exactly on one. Nothing
here needs the player to hold still anywhere, which is what the three
attempts before it foundered on.

**Then the fix**, and it is now a known shape: substitute `cb2` for exactly
the movie's two composite draws, with the values the splash uses. That is
`panel_distance`'s own mechanism -- a composite's constants swapped for one
draw and restored after -- pointed at a different slot and a different draw.
The ordering problem (the movie plays first, so the splash's numbers have
not been seen yet) is unchanged and is decided by what flight 6 returns: a
derivable relationship, a value learned and kept, or a rule read off the two.

The superseded plan, kept because its reasoning was sound and its target
was wrong:

**Flight 5 — the uv span, for the movie and for the splash.**

Two launches, or one and a re-arm. The movie's:

```ini
[advanced]
quad_probe = 5424x5356:X:6:120
quad_probe_at_ms = 0
intro_probe = 0
```

and the splash's, on the same composite after the movie has ended:

```ini
quad_probe = 5424x5356:X:6:60
quad_probe_at_ms = 45000
```

The target is the **composite** again, not the fill — flight 4 settled the
fill and it is full-surface, so whatever shrinks the picture is on the side
that READS the surface. Its uv span is the number, and the probe now
reports it.

`quad_probe_at_ms` is what makes the splash reachable without anybody
waiting. Flight 3 tried a live re-arm at the menu and lost the session
first; flight 4 tried a frame count and landed nineteen seconds early
because the rate was 178 fps, not the 62 the clock's resolution had implied.
45 seconds is comfortably past a twenty-second movie that starts around
six, and the small skip after it just avoids the first frame of the menu.

If the two spans differ, the difference is the fix. If they are the same,
the picture is not sized here at all and the next place to look is the
composite's pixel shader — dumpable with `glare_shader_dump = 1`, which
would make it a reading exercise rather than another flight.

**Then the fix.** Its shape depends on which mechanism flight 4 names —
a viewport is substituted at `RSSetViewports`, four vertices are rewritten
the way `panel_quad.cpp` and `loader_panel.cpp` already rewrite a matched
draw's geometry. Either way the values written are **the splash's own**, not
anybody's chosen placement, which is what makes the cut from movie to splash
land. The draw is recognised by shape and by what it samples (a four-vertex
draw into a 1920x1080 surface whose sources are three R8 planes at
1920x1080, 960x540 and 960x540), not by a game version.

One ordering problem to solve when it is written: **the movie plays before
the splash does**, so the geometry to copy has not been seen yet when it is
first needed. Three ways out, in order of preference, to be decided against
the measurement rather than now — the two turn out to differ by something
derivable (a fit the source aspect implies); or the splash's geometry is
learned in one session and kept; or the rule behind both is read off the two
measurements. If they turn out to be identical, the premise is wrong and
this page says so.

**And possibly a second fix**, for facing the wrong way at the cut. Not the
same mechanism, not the same measurement, and it should not be folded into
the panel work until it has one of its own.

**What the fix will not do:** phase A, and the freeze. Both are the game
bringing VR up, before it has handed the compositor anything.

**Not on the table: skipping the movie.** It is the game's content, the field
asked to see it properly rather than to lose it, and a patch that presses
Escape on somebody's behalf is a patch deciding what they get to watch.

## The instruments (both `[advanced]`, off by default, live)

| key | does |
|---|---|
| `intro_probe = 1` | records the startup. Per frame: draws counted by the target they land in, logged when that shape changes; every frame of 200 ms or more; every new clear colour, read *before* the black void fix substitutes; and the first frame that draws into an eye texture, called out on its own line. With the movie not skipped, the same forwarding file-open hooks the skip uses are installed at launch (`intro probe: watching the movie's open`), so the movie's open (`the game opened Ident_... at +X s after the device`) and its first and last fill are timed against the device line; the watch's account prints at the first rendered scene, and a reload after the movie has drawn declines it and says so. Refuses nothing; stands down after three minutes or 96 lines. |
| `census_at_ms = A,B,C` | arms a full draw census at named moments after the session's first frame — up to eight, milliseconds. Each records offscreen draws whatever `census_offscreen` says. Read once, at the first frame. An entry due inside a stall fires when the stall ends. |

Milliseconds and not frames, deliberately: this session measured 1130 fps
through the startup's empty phase and 60 through the movie, so one frame
number is a different moment on every rig and in every session.
`common/timing.h` is that rule, and this is the instrument that most wanted
to break it.

Both count frames the same way `DC` lines do — `intro probe: frame N` and
`DC begin ... frame=N` are the same N.

## What shipped

All four keys are in `[fix]`, all off by default, all live.

| key | what it does |
|---|---|
| `intro_video_size = stock \| splash \| <number>` | `splash` grows the movie's panel until it covers what the splash covers, derived at runtime from the panel's own constants -- x5.53 on the measured rig. A number is a plain multiple. |
| `intro_video_lock = head \| world` | `world` replaces the panel's screen-space transform with a real view-projection, so the movie is drawn on the splash's own screen: 16:9, 3.35 m away, on the game's forward, with the stereo a screen at that distance has. |
| `intro_video_upscale = stock \| sharp \| fsr` | `fsr` is AMD's EASU from their vendored sources, `sharp` a Catmull-Rom, and `sharp` is also the automatic fallback if EASU will not compile. Resamples to `vscreen_res_width`, floored at twice the source. |
| `advanced.intro_video_deband / _dither / _sharpen` | the deband ahead of the upscale, and AMD's RCAS after it. |

The order is deband, then EASU, then RCAS: the blocking is in the encode and
lives in source space, so it is flattened before anything magnifies it; RCAS
after the upscale is AMD's own order.

**What it does not do.** Phase A -- the movie plays for three to five seconds
before Elite hands the compositor anything, and no frame of that is ours to
change. The 2.7 s freeze, which is the game bringing VR up. And the game's
forward being 180 degrees out on this rig, which the handover pose log
established belongs to the runtime and not to Elite.

## What the bugs were, since the pattern is the lesson

Four of the last six flights failed on the same *kind* of mistake rather than
on the physics, and they are worth listing together:

* **A viewport counter-move to world-lock the panel.** It can only translate,
  so it can never give stereo, so the picture always reads as being at
  infinity. It could not have worked at any sign or gain -- and I shipped a
  gain knob to tune the sign of something structurally incapable.
* **The `fsr` mode never parsed.** The three-mode rewrite failed its
  assertion inside a batch of edits and I re-ran only some of the others. The
  first-call path then returned without logging, so the log was silent rather
  than wrong. *When a batched edit aborts, every edit in that batch is
  suspect.*
* **The resample cache keyed on the source resource.** The two eyes have
  their own surfaces, so each eye's draw destroyed the other's build and the
  second bound a texture nothing had dispatched into. Fixed, then
  **reintroduced one commit later** with a comment explaining why it was
  fine. The comment was half right -- the source view genuinely must follow
  the resource; the chain must not. Two lifetimes, one cache test.
* **The deband threshold carried across with the algorithm.**
  `backdrop_fix`'s 8/255 is tuned for BC1 banding, which steps 4-8 levels.
  DCT blocking at 2.9 Mbit/s steps much further, so the weight was zero at
  every block edge and the pass preserved them exactly. A number is part of a
  measurement, not part of an algorithm.

The physics was read correctly at every stage -- from the shader
disassembly, the constants, the frustum tangents. What went wrong was
assumed: a handedness, a lifetime, a threshold, a mode string.

## Not playing it at all: `fix.intro_video = skip` (built 2026-09-13, unflown)

Sean's ask: a fix option that skips the intro video completely, with the
default left as the movie playing. `skip` is the third value of the existing
key rather than a key of its own, because it names what the player gets.

**The mechanism** (`src/d3d11/intro_skip.cpp`). The idents are files under
`Movies\`, opened by path, and the long-standing way to skip them is to
rename the file so the game cannot find it. The skip does the same thing
in-process: the executable's own imports of `CreateFileW`, `CreateFileA` and
the `GetFileAttributes` family (iat_hook.h, the keyboard gate's mechanism)
are watched, and a path whose last two components are `Movies` and
`Ident_*` or `intro_temp.webm` is answered `ERROR_FILE_NOT_FOUND`. Nothing on
disk changes; there is nothing to restore after a crash, a game update or an
uninstall, and `FrontEnd*.webm` -- the front end's own loops -- pass through.
The hooks are installed the first time the value is seen (at device creation,
before the movie's open at one to three seconds) and only then; `screen` and
`stock` never touch the import table. `skip` parses as `screen` for every
other slice, so if the refusal does not take, the movie plays on the splash's
screen as before, and -- the point -- the fill detection stays armed as the
skip's witness.

**What is not known, and the one flight that decides it.** Two things were
measured at the desk and two were not:

- *Measured:* the executable names `webmsplit64`, `vp8decoder64` and
  `dsfVorbisDecoder64` itself and carries `DllGetClassObject`, `Movies/` and
  `.webm` strings -- it builds the DirectShow graph by hand. `webmsplit64.dll`
  is a splitter ("WebM Splitter", no source-filter name), so it does not open
  the file. The executable imports `CreateFileW` and `CreateFileA` from
  `KERNEL32.dll` (the NUL-delimited import names are in its table; the
  `GetFileAttributes*` names could not be confirmed by the same scan, so those
  four hooks report `not imported` if the slot is absent, which is itself a
  fact).
- *Not measured:* **which module makes the open** -- the executable through
  its import table (this hook sees it), its C runtime (it does not), or a
  DirectShow file source of Windows' own such as quartz's async reader (nor
  that). And whether the game's missing-file path is a clean skip is the
  field's word, not this project's measurement.

The log says which of three things happened, once, when the first rendered
scene arrives:

| line | meaning | next |
|---|---|---|
| `intro skip: WORKED -- N refusal(s), the movie never drew, and the first rendered scene arrived X s after the skip armed` | done; X against the ~28 s the movie costs is the win | ship |
| `intro skip: the movie is DRAWING anyway -- ... NOTHING was refused` | the open went by a route the executable's import table does not carry | a process-wide hook: `code_hook.h` on `KernelBase!CreateFileW`, whose prologue on this machine reads `48 8B C4 48 89 58 08` -- `mov rax,rsp; mov [rax+8],rbx`, seven relocatable bytes -- so the detour would be accepted; `kernel32!CreateFileW` is a `jmp [rip+…]` forwarder the decoder refuses, and hooking KernelBase covers it too |
| `intro skip: the movie is DRAWING anyway, after N refusal(s)` | told no, got the file anyway (a retry by another route) | the same process-wide hook, or a look at what the retry is |
| `intro skip: no ident was asked for ... and no movie drew (M other Movies\ open(s) seen)` | neither refused nor drawn; M says whether this table sees the game's movie opens at all | read M before guessing |
| `intro skip: did not take -- N refusal(s), the movie drew F frame(s)` | refused, drew, scene | as the DRAWING line |

A game hang or crash at launch with `skip` set is the fourth outcome and the
one that refutes the field's word about the missing-file path; the crash
sentinel turns the d3d11 half off for the following launch, which removes the
hook, so a rig cannot be left stuck on it. The next design in that case is a
redirect rather than a refusal: answer the open with a one-frame WebM of
EDVR's own, so the game plays a movie that is over at once.

The flight can be a flat launch -- the intro plays without a headset and the
d3d11 half loads regardless -- with `fix.intro_video = skip` in the ini
before launch. The number to compare the WORKED line's seconds against is,
from a `screen` session, the `intro video size: a rendered scene arrived`
line's timestamp less the log's first line's.

## Flight 09:43: the file reader bypassed the executable hook

Steam log 094305 verifies fbd8284, linked 15:36:15 UTC. Skip armed at
09:43:05.456, the movie drew at 09:43:09.750, and the first scene
arrived 28.8 seconds after arming. There were zero refused requests,
zero other Movies requests, and 902 movie frames. The configured value
was `skip`.

Ruled out: the option was disabled or the installed build lacked the
feature, because the matching run explicitly armed its executable
imports. Ruled out: intercepting only the executable's file imports is
sufficient, because playback completed without any request reaching
them.

The installed executable contains CLSID_AsyncReader
`E436EBB5-524F-11CE-9F53-0020AF0BA770` at file offset `0x56346e8`.
Windows' implementation is in `quartz.dll`, which imports CreateFileW
itself. An isolated process loading the actual EliteNeutral ident
through IFileSourceFilter reproduced the result: the executable import
hook saw zero calls and Load succeeded. Hooking the reader module's
import saw three successful opens. Refusing the ident at that import
made Load return `0x80070002`, file not found, immediately. This is a
measured reader path, not a guess based only on the video's continued
playback.

Skip now loads the system copy of quartz before graph setup and patches
its CreateFileW import as well as the existing executable imports. It
holds the module reference until shutdown restores the import. Screen
and Stock remain unchanged, and other movie paths still forward. The
change does not detour KernelBase or modify files. The shared IAT helper
now accepts an explicitly owned module; it publishes the original
function before atomically installing the replacement, so a reader
thread cannot enter a hook with an uninitialized forward pointer. A
competing slot change causes the install to stand down.

The new build-gated DirectShow regression uses temporary fixtures and
the real Windows reader. It proves the executable-only miss, ident and
alternate-intro refusal, allowed front-end and story videos, live
disarming/rearming, original-slot restoration, unchanged files and the
success-report path. The existing predicate/refusal smoke tests remain.
The next game launch must still confirm the game's missing-file
handling: look for `DirectShow reader CreateFileW patched`, a refusal
naming `DirectShow CreateFileW`, and `WORKED` with no movie frames. The
standalone reader test cannot by itself establish Elite's scene
transition behavior.

## Flight 10:09: the quartz.dll fix confirmed, and a false alarm explained

Steam log `edvr_gfx_20260913_100913.log` verifies v0.16.1-14-g2aa0c2d,
linked 2026-09-13 16:05:14 UTC. Skip armed at 10:09:14.213; the
DirectShow reader's `CreateFileW` refused
`Movies/Ident_Frontier_EliteNeutral.webm` twice, at 10:09:17.620; and
`intro skip: WORKED -- 2 refusal(s), the movie never drew, and the
first rendered scene arrived 22.5 s after the skip armed` printed at
10:09:36.745. Against the 28.8 s Flight 09:43 measured when the movie
played in full, that is about 6 s saved to first scene, not the ~20 s
the movie itself runs (the WORKED line's own parenthetical). A
near-identical run the same morning, log `edvr_gfx_20260913_100228.log`
on the same build, also WORKED: 2 refusals, scene at 22.3 s.

This is the confirmation the doc's last entry asked for. The log's
version line verifies build 2aa0c2d, which patches quartz.dll's own
`CreateFileW` import before graph setup -- closing the gap Flight
09:43 found. And the game's missing-file handling, which Flight 09:43
left as "the field's word, not this project's measurement," is now
measured directly: two refusals, no movie frames, no hang, and a
clean scene transition.

One line in the same log reads oddly: `intro skip: the movie is
DRAWING anyway, after 2 refusal(s): ... Please report this log`, at
10:09:37.330 -- 0.585 s after WORKED. Commit 0d7251b (2026-09-13, the
same day) explains and silences it: the front end's own background
loops (`FrontEnd*.webm`) ride the same YUV composite the fill detector
matches, and start once the scene is up, so a fill counted after the
verdict is that loop, not a second ident replay. "Log discipline only
... no behaviour changes" per the commit message. Both 2aa0c2d and
0d7251b are in tag v0.16.2 (`git merge-base --is-ancestor 2aa0c2d
v0.16.2` confirms the first; the tag's own tip is 0d7251b itself).

Not yet flown: the fourth outcome (a hang or crash with `skip` set,
per the table above), and whether the Frontier install's missing-movie
path behaves the same -- Flight 09:43 and Flight 10:09 are both the
Steam rig.

## 2026-09-15: the first submitted frame's 857 ms, and the NGX warm-up

Steam flight, `edvr_gfx_20260915_183004.log`, v0.17.0-rc.1-3-g3b6715a-dirty,
Pimax Crystal Super on Pimax OpenXR, `fix.temporal_aa = dlss`. Times are
the log's own clock (mm:ss.fff).

| t | line |
|---|---|
| 04.28 | device created |
| 05.35 | VR_InitInternal -> `runtime_startup` at 06.23 (the OpenXR session is built synchronously in here; the Present thread is parked servicing graphics jobs) |
| 08.41 | first WaitGetPoses / Submit |
| 08.411 | `adaptive UI evidence ready` -- the first treat starts (ensureUiHistory) |
| 09.170 | `UI resolve: replacement compute shader compiled` -- the runtime D3DCompile |
| 09.228 | `dlaa: the feature is created for eye 0 at 4068x4016, DLAA, preset K` |
| 09.265 | `... eye 1` (37 ms later) |
| 09.268 | `native timing CPU: seq 3, wait 7.077 ms, submits 857.166 ms, temporal 853.701` |
| 09.479 | `intro video upscale: FSR ... 1920x1080 to 5120x2880` -- the movie's first EDVR composite |
| 09.721 | `intro video size: engaged -- x4.15`, world lock bound |
| 10.524 | the game resizes its targets to 2644x2610 (HMD Quality 65%) |
| 10.556 / 10.586 | `dlss: the feature is created for eye 0/1, 2644x2610 in and 4068x4016 out` -- ~30 ms per eye; LONG FRAME 107.8 ms |

The Frontier 10:45 flight is the same shape: first Submit 780 ms, temporal
776.8 ms.

**Attribution, corrected.** The first reading put the ~800 ms on eye 0's
NGX feature creation. In code order the first treat runs ensureUiHistory
(08.411) -> `dlaaAvailable` = NGX initialisation -> the pass's five
4068x4016 dl textures -> the UI-resolve runtime compile (09.170) -> eye 0's
`NGX_D3D11_CREATE_DLSS_EXT` (09.228). So the create itself is <= 58 ms
(09.170 -> 09.228; eye 1's was 37 ms; the later 65% recreate ~30 ms per
eye). The ~760 ms before it is NGX init, the five 16-MP textures and the
UI-resolve compile, unsplit -- nothing in that log times them apart. It is
a one-time warm-up cost, not a per-size one.

**The design, in five lines.**

1. `temporalPassTick` runs on every non-TEST Present of the game's
   swapchain, on the game's immediate context, on the render thread --
   the thread and device the native temporal channel's `treat()` insists
   on. From the Present after VR_InitInternal's park ends (~06.24) it can
   see the published render size and the acquired channel, ~2 s before the
   first Submit.
2. `warmTrainedOnce` (temporal_pass.cpp) gates once per session on: a
   trained mode, `advanced.temporal_aa_warm`, no debug paint, a valid
   published sizing (`edvrQueryNativeRenderSizing`), the channel acquired,
   this thread == the channel's thread, this device == the channel's device
   (IUnknown identity), the device not removed, the size in range. Every
   gate fails closed to today's behaviour: the sizing, channel, thread and
   device-identity checks poll (Pending until they open); a trained mode,
   the warm switch and debug paint are terminal to Off; a removed device
   or an out-of-range size are terminal to Failed.
3. `dlaaWarm` (dlaa.cpp) calls `dlaaAvailable` (NGX init, latched exactly
   as the treat's call latches it) then `ensureFeature` for eye 0 and eye 1
   at w = h = out = the max over both eyes' active sizes -- the value
   `GetRecommendedRenderTargetSize` hands the game, which creates both eye
   textures at it (4068x4016 tonight).
4. `ensureFeature` is the creation block lifted verbatim out of
   `dlaaEvaluate`, which now calls it: the first treat finds the handle set
   and every key term equal and goes straight to the evaluate with
   reset = true; on a mismatch (Frontier's 65% first frame, HMD Quality, a
   cull-guard-widened width, a preset change) it releases and recreates at
   the measured ~30-50 ms per eye -- the init saving stands either way.
5. Every stage is timed whether or not the warm-up runs: `dlaa: NGX
   initialised in N ms`, `(made in N ms)` on both `created for eye` lines,
   `(N ms)` on every `replacement compute shader compiled` line. Not made
   by the warm-up: the fovea/periphery crop features, the pass's dl
   textures and mover pair (keyed on the treat's real size and the game's
   swapchain format), any Evaluate.

| key | what |
|---|---|
| `advanced.temporal_aa_warm = 1` | NVIDIA initialised and both eyes' DLAA features made on the loading screen, once openvr_api.dll has named the render size; 0 = the first frame pays it, as before. Restart. |

**The never-ran case is distinguishable by construction.** With the tick's
call deleted, the first treat prints `temporal aa: NVIDIA was not warmed
before the first submitted frame (no frame boundary reached the warm-up:
the tick never ran)` and the `created for eye` lines still sit after
`adaptive UI evidence ready`.

### 9. Flight reading

FIRST: `python tools\edvr_log.py --target steam --expect-build HEAD --version`
-- exit 2 means a stale DLL and nothing below is evidence. Then read the gfx
log in this order, with the openxr log beside it.

A. DID IT RUN AT ALL. Look for `temporal aa: warming NVIDIA before the first
submitted frame` (L-START). Absent, and at the first treat `temporal aa:
NVIDIA was not warmed before the first submitted frame (<reason>)` (L-LATE)
= it never ran; the reason says why: `the tick never ran` = dead code (the
`precompiled shader warmed at session start` canary will also be missing if
the tick itself is dead); `openvr_api.dll had not published a native render
size` or `the native temporal channel was not acquired` = the gate never
opened -- check the openxr log's `openxr_render_size` and
`native_temporal,provider_acquired=1` stamps against the first boundary
after runtime_startup; `advanced.temporal_aa_warm = 0` = off (L-OFF also
printed). In the never-ran case the `dlaa: the feature is created for eye
0/1` lines still sit AFTER `adaptive UI evidence ready` and BEFORE the first
`native timing CPU: seq` line, and `dlaa: NGX initialised in N ms` (L-NGX)
is stamped in the same frame -- read that N and the `(N ms)` on `UI
resolve: replacement compute shader compiled` anyway: they split tonight's
760 ms even on a flight where the warm-up never ran.

B. RAN AND HELPED. L-START, then the two `dlaa: the feature is created for
eye N at WxH ... (made in N ms)` lines, then `temporal aa: NVIDIA warmed
before the first submitted frame -- initialisation A ms, eye 0 B ms, eye 1
C ms ...` (L-DONE), ALL stamped before the first `native timing CPU: seq`
line (and before the openxr log's first native_submit_route seq /
WaitGetPoses). At the first treat: NO new `created for eye` line (the pair
was reused), `DLAA engaged` prints as before, no L-LATE. `native timing
CPU: seq 3 ... temporal` well under 150 ms (from 853.7). AND in the openxr
log the gap runtime_startup -> first native_submit_route stays ~2.2 s
(tonight 06.228 -> 08.411): the game did not absorb the stall. Then the
movie: `intro video upscale: FSR` and `intro video size: engaged` should
land earlier relative to the `device` line than tonight's +5.20 s / +5.44 s
by roughly the fall in `temporal`.

C. RAN, DID NOT HELP (three distinguishable shapes). (1) L-DONE present,
features reused, but `native timing CPU: seq 3 ... temporal` still hundreds
of ms: the residue is not NGX -- compare L-DONE's initialisation A to ~750:
a small A means the cost is the five 16-MP dl textures or the UI-resolve
compile (its line now carries ms; subtract it) -- write the ruled-out line
and pre-build whichever it is next. (2) L-DONE present and temporal small,
but the openxr gap runtime_startup -> first native_submit_route grew by
about L-DONE's total (toward ~3.0 s): the Init thread waited on the render
thread; the stall moved, the movie is no earlier -- record it, the next
lever is init off the render thread (needs a decision: the game device is
render-thread-only by house rule). (3) L-DONE present but a SECOND `created
for eye 0` line at the first treat at a different size (e.g. 2576x2544 ->
3964x3914 on Frontier): the size guess was wrong; the init saving stands,
the recreate's `(made in N ms)` prices the loss; note the sizes for the
DLSS-fraction follow-up. Also: `temporal aa: NVIDIA warm-up did not
complete ... -- <reason>` (L-FAIL) or `... faulted` (L-FAULT) = it ran and
refused; the treat retried; read the reason and, if init refused, the
treat's `dlaa was asked for, but` line. The monitor's LONG FRAME row may
not print for the warm frame (the VR_Init line consumes the 5 s budget) --
its absence is not evidence either way; L-DONE's totals are the record.

**Open after the flight (not in this change):** pre-creating the
DLSS-fraction pair for rigs whose first engage is 65% (Frontier) from the
auto bias source's multiplier; pre-building the pass's dl textures if the
split says the 325 MB block is the cost; precompiling kUiResolve in
build.bat's temporal shader step if its compile ms is large; NGX init off
the render thread if the Init thread turns out to wait on the render thread
in the window; releasing abandoned warm features on the format-family
fallback.

ruled out: (to fill after the flight)

## 2026-09-15: the native runtime, and what replaced the early handover

**The premise correction.** `fix.vr_handover = early` (removed 2026-09-13)
submitted a 1x1 texture ahead of the game's own compositor calls under
OpenComposite, whose session rebuilds at the first such call. The native
OpenXR bridge (`src/openxr`) has no such rebuild: the whole session --
instance, EDVR's own D3D11 device, `xrCreateSession`, swapchains, runtime
shader compiles, `xrBeginSession`, two zero-layer frames, launch centre --
is built synchronously inside `VR_InitInternal` on an OwnerService thread
while the game's Init thread waits and its Present thread is parked in
the Present hook servicing graphics jobs (native_module.cpp:301-367,
native_runtime_host.h:424-467). There is nothing for an early handover to
hand over to.

**Tonight's Steam flight** (`edvr_gfx_20260915_183004.log`,
v0.17.0-rc.1-3-g3b6715a-dirty, Pimax Crystal Super on Pimax OpenXR; times
are the log's own clock, mm:ss.fff):

| t | line |
|---|---|
| 04.28 | device created |
| 05.35 -> 06.23 | `VR_InitInternal` -> `runtime_startup` |
| 08.41 | first WaitGetPoses / Submit |
| 09.228 / 09.265 | `dlaa: the feature is created for eye 0/1 at 4068x4016` |
| 09.268 | `native timing CPU: seq 3, wait 7.077 ms, submits 857.166 ms, temporal 853.701` |
| 09.479 | `intro video upscale: FSR ... 1920x1080 to 5120x2880` |
| 09.721 | `intro video size: engaged -- x4.15` (world lock bound, no refusal) |
| 10.524 | game resizes to 2644x2610 (HMD Quality) |
| 10.556 / 10.586 | `dlss: the feature is created for eye 0/1 ...`; LONG FRAME 107.8 ms |

The movie plays natively on the splash's own screen, world-locked, at
+5.44 s after the device (the FSR composite lands at +5.20 s), with no
refusal line. The only EDVR-owned cost on that path is the 857 ms first
Submit, of which ~800 ms is eye 0's one-time NGX warm-up -- eye 1 costs
37 ms, the later 65%-quality recreate ~30 ms per eye -- and the game's
own first compositor call costs ~7.077 ms (`wait`), not a rebuild. The
Frontier 10:45 flight shows the same shape (780 ms first Submit, 776.8 ms
temporal). The 4068x4016 eye size is the runtime's own recommended
per-eye size, published at Init and read back by the d3d11 half
(`vScreen: openvr_api.dll says one eye is 4068x4016`).

**Built** (log-only; no ini keys besides the warm-up's; `src/openxr`
untouched by the instrumentation; nothing committed):

| instrument | adds | signature |
|---|---|---|
| I1 ident watch | non-refusing timing of the movie's open, `advanced.intro_probe` | `intro probe: watching the movie's open -- the file hooks are installed and forwarding ...` |
| I2 fill timing | first/last movie-fill draw vs the device clock | `intro probe: the movie's fill first drew at +X.XXX s after the device` |
| I3 world-lock confirm | anchor yaw at bind, said once | `intro video lock: holding -- the panel is anchored on the game's forward (head yaw N deg at bind, panel frame N, left/right eye first)` |
| I4 startup steps | `VR_InitInternal`/`start()` split into instance/device/session/swapchains/shaders/other/frames/centre | `runtime_startup_steps,instance=,device=,session=,swapchains=,shaders=,other=,frames=,centre=,total=,units=wall_ms` |
| I5 skybox counts | two fields on the existing summary line | `native_summary,...,skybox_sets=<n>,skybox_clears=<n>` |
| I6 present park | Present-thread cost of the runtime-start invoke | `present_park,ms=<n>,jobs=<n>,reason=runtime_start,result=<EVRInitError>,path=<present_queue\|inline>` |
| warm-up | `advanced.temporal_aa_warm` (default on) | `temporal aa: NVIDIA warmed before the first submitted frame -- initialisation A ms, eye 0 B ms, eye 1 C ms ...` (L-DONE) |

**Flight reading, in order.** First `edvr_log.py --expect-build HEAD`:
exit 2 kills the log as evidence. Then: did the warm-up run at all
(L-START present, or L-LATE naming why -- dead tick, the gate never
opened, or off); if it ran, did it help (L-DONE before the first `native
timing CPU: seq` line, that line's `temporal` well under 150 ms, and the
openxr gap `runtime_startup` -> first `native_submit_route` unchanged at
~2.2 s); or did it run and not help, in three shapes -- the residue
survives reuse (not NGX: the dl textures or the UI-resolve compile), the
gap grew by about L-DONE's total (the Init thread waited on the render
thread; the stall moved, not removed), or a second `created for eye 0`
line at a different size (the size guess was wrong, the init saving
still stands).

**Deferred, not built:** precompiling the openxr half's three D3DCompile
pairs to shrink `VR_InitInternal`'s ~443 ms shader stretch; an EDVR
holding layer during the ~2.2 s between `runtime_startup` and the first
Submit (the first pre-game `xrEndFrame` layer on third-party runtimes).
Both wait on the warm-up's own measurement landing first.

## 2026-09-17: a skipped intro left the panel armed, and the on-foot HUD passed for the movie

Sean, after the 09:47 Steam flight (v0.17.0-rc.3-69-g00c27ed, weapon-light
work, on foot from the first minute): "the virtual panel is set further
away than what's in the ini and it is no longer curved", and the frame
slower. Nothing in vscreen had changed; the panel fixes died because the
composite they key on stopped reading the panel.

**What the log said** (08:08 and 09:47 flights, identical shape):
`vScreen: panel distance x0.700 applied` at the first on-foot composite,
then 0.4 s later `intro video upscale: FSR (AMD's EASU) -- 5120x2880 to
8192x4608, ... and the composite samples ours`, then `intro video size:
the panel's constants do not read as a screen-space placement` every
frame (1094 lines in 09:47), `panel distance applied` climbing ~29 per
20 s instead of ~2 per frame, and `the panel's transform buffer has gone
600 frames unused` three times (the cap). The intro was skipped in every
flight (`intro skip: WORKED ... the movie never drew`).

**Cause.** `introPanelTick` retired the panel at the first rendered scene
only if it had matched a draw first (`g_slotCount || g_applied`, from
2850ccf). A skipped intro matches nothing, so the fill signature (a
four-vertex draw with PS slots 1 and 2 bound into a Texture2D) and the
composite signature (six vertices, one instance, SRV0 the fill's size)
stayed armed all session. The on-foot HUD satisfies both -- a
four-vertex draw into the 5120x2880 panel, then the panel's own
composite -- so from the first on-foot frame the resample chain
substituted its 8192x4608 output for the panel at the composite.
`beginPanelOverride` recognises that composite by SRV0 being the panel
(`srv0IsPanelSized`), so distance and curvature stopped applying, and
the chain's deband + EASU + RCAS ran at 8192x4608 every frame on top.
The on-foot eye-draw count sits under `kSceneEyeDraws` (100), so no
later frame retired it either (08:08 retired at 08:11:55, a frame that
crossed 100; 09:47 never did).

**Why it never showed before.** Ship-first sessions (06:39 same day,
2026-09-16 17:39) made the same false match in the ship at 06:41:39 --
`intro video upscale: the movie's frame could not be viewed unconverted`
(an R8G8B8A8 view refused on that target) failed the chain for the
session, and with a scene on screen the panel retired the same frame,
"It resized 0 draw(s)". Luck, not design.

- ruled out: a vscreen regression, because vscreen.cpp, intro_panel.cpp
  and intro_upscale.cpp are unchanged across 2ebed2d..eefcb09, and the
  06:39 flight (2ebed2d-dirty) shows the same false match surviving only
  by the view failure.
- ruled out: the performance drop as this alone, because Sean also
  raised `fix.openxr_resolution` 3900 -> 4100 at 08:08:58 (+10.6 %
  pixels, DLSS 1.93 -> 2.25 ms per pair); the chain's three passes at
  8192x4608 per frame come on top of that.

**Fix (this entry's commit):** retire at the first rendered scene
unconditionally, the rule the header and `loader_panel.h` state; the
skipped case logs `intro video: a rendered scene arrived and the movie's
panel was never seen ... stand down for the session`. The movie case is
unchanged: it matches during the movie and retires at the scene after
it. Built green, NOT FLOWN.

**Next flight (Steam, skip armed, go on foot):** the new line at the
first rendered scene (~20 s after launch, before LoadGame); no `intro
video upscale: FSR (AMD's EASU) -- 5120x2880` line on foot; no
`constants do not read` lines; `panel distance applied` climbing ~2 per
frame in every `vScreen totals` line while on foot; no `600 frames
unused` while the panel is visible; and the panel back at 0.7 m, curved.
If the new line is missing the tick never saw a scene frame -- the
`intro skip: WORKED` line shares that boundary and would be missing too.

## 2026-09-17: the intro/splash "forward" is an unstable one-shot recentre

Sean: not consistently facing the intro screen (movie or splash) at
launch -- "sometimes I seem to have it show up to either the left or
right of me". The panel's world-lock (`fix.intro_video_lock = world`,
`intro_panel.cpp`) counter-moves against "the game's forward" -- the
same forward the game's own splash anchors to, by design, so one
recentre fixes both (the comment above `g_worldLock`). That forward is
set once, by `NativeRuntimeHost::centreAtStartup`
(`native_runtime_host.h:1457`), inside `VR_InitInternal`'s startup poll
loop, the first time a valid+tracked, non-placeholder pose arrives
(`LaunchCentrePolicy::consider`, `launch_centre_policy.h`) --
`seatedOriginFromHead` takes that pose's yaw (levelled, position too)
as the new seated origin. No settle window, no stillness check: whatever
direction the head is pointed at that instant is what forward becomes.

**Measured from existing logs, no new flight needed.** Ten of today's
Frontier `edvr_openxr_*.log` files' `native_launch_centre,applying=1`
lines, yaw recovered from the logged orientation quaternion the same
way `seatedOriginFromHead` does (atan2 of the local -Z axis projected
onto the XZ plane): -16.1, 54.1, 61.2, -2.8, 5.4, 13.9, 3.1, 11.8, -3.0,
1.2 degrees -- a ~77 degree spread across sessions on the same
headset, every one `flags=15` (fully valid and tracked, not the
identity placeholder). Two of the ten took 9 poll samples (~90 ms
apart) before the first usable pose; the rest fired on sample 1.

The newest session's own `intro video lock: holding` line (the panel's
bind-time diagnostic, `intro_panel.cpp:588`) reads "head yaw 73.6 deg
at bind" -- by the time the panel actually appeared, about six seconds
after that session's recentre (matching the doc's measured ~5 s to
first movie draw), the head had already turned 73.6 degrees away from
the forward the recentre had just set. The user cannot see anything
to hold still for at recentre time, and nothing renders for several
seconds after.

**Why this is the "facing the wrong way after the cut" item, not new:**
the 2026-08-28 entry measured a single fixed 180-degree offset on one
rig and read it as a play-space peculiarity. Ten flights on the same
rig today show the offset is not fixed -- it tracks whatever the head
was doing near the very first tracked pose, plus however far it drifts
before the movie or splash can be seen. Both are anchored to this one
seated origin on purpose, so the fix belongs in the origin capture, not
a panel transform -- "prefer root causes to compensation".

- ruled out: nothing here -- a new finding, not a refutation.

**Sean's call:** capture as late as practical -- recentre once the movie
starts playing, or, if it is skipped, once the splash after it is
visible -- rather than adding a settle window to the startup capture.

**Built (14ec395's follow-up, NOT FLOWN).** A new cross-DLL channel,
`requestIntroRecentre` / `introRecentreRequested` /
`clearIntroRecentreRequest` (frame_flag.h/.cpp, mapping bumped to
_v33): d3d11.dll asks, openvr_api.dll acts. `intro_panel.cpp`'s
`introPanelOnComposite` fires it once, the first time it sees the
composite's own shape (`kind=='X', count==6, instances==1`) while
`!sceneArrived()` -- movie or, if skipped, splash, the same draw
(`splash_dim.h`'s "same geometry... same draw call" holds for the
detector too, not only the dim). Gated on `!sceneArrived()` rather than
the movie's fill match specifically, on purpose: the fill never happens
at all when the movie is skipped, and the on-foot HUD's own six-vertex
composite (the 2026-09-17 false-match entry, same day) is excluded by
the scene boundary instead, the already-hardened one `loaderPanelTick`
and the panel's own retirement already rely on. `NativeRuntimeHost`
polls the request at the top of `waitPoses`, between frames as
`centreAtStartup` requires, and calls the SAME `applySeatedReset` the
manual recentre hotkey uses (`notifyGame=true`, so Elite gets the usual
recentre VREvent) -- reusing rather than duplicating the mechanism. A
poll that cannot act this frame (a frame open, or the reset-event queue
full) leaves the request set and retries next frame rather than losing
it. Known gap: `fix.intro_video = stock` (world-lock AND upscale both
off) never calls `introPanelOnComposite` at all -- its caller gates on
`introPanelWants()` -- so stock-mode users keep today's behaviour; not
folded in, since stock is an explicit opt-out of the whole treatment.

**Next flight:** Frontier, default settings, watch for `intro video:
first screen composite -- asking the vr half to recentre` (once) paired
with a recenter taking effect -- no direct log line confirms the
openvr-side apply succeeded; look for it in the SAME session's
`edvr_openxr_*.log`, `native_launch_centre,applying=1` earlier and no
`geometry_invalidated,reason=intro_recentre` missing right after the
gfx line's timestamp. The real test is subjective: does the movie or
splash now face you. Try it with the headset settled AND unsettled
(mid-adjustment) at launch, across a few relaunches, to see whether the
spread the ten flights showed is actually gone.
