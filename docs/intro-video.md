# The intro movie

Reported 2026-08-28 and **measured the same day**, across two flights, on the
field rig (Frontier launcher install, game build 330683, one eye 5424x5356,
Pimax via OpenComposite). The model this page opened with was a hypothesis
built from the install's own files and from old logs; flight 1 confirmed it,
including the vertex shader, and flight 3 then refuted the part of it that
said where the fix would go. Both are below, because the correction is the
finding. No fix is written.

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

**Flight 4 — where the picture sits inside the surface, in BOTH states.**

```ini
[advanced]
quad_probe = 1920x1080:N:4:1200
census_at_ms = 4000
intro_probe = 0
```

The target is no longer the composite — flight 3 settled that one and it has
nothing in it. It is the draw that FILLS the composite's source: a
four-vertex `N` into the 1920x1080 surface, one per eye, from the three YUV
planes. The capture logs its viewport, its scissor and its four vertices,
which between them say whether the picture occupies the whole surface or a
rectangle inside it, and by which mechanism.

The skip count is the trick that gets **both** states from one launch.
Matching frames accrue at ~62 fps from the handover at 6.4 s, and the movie
runs about twenty seconds; `1200` is therefore about nineteen seconds of
them, so the capture lands *after* the movie — at the menu, on the splash,
which reaches the eyes through the same surface and the same composite.
Flight 3 tried to get the second state by re-arming the probe live while the
player waited at the menu, and lost it when the session ended first; a skip
count needs nobody to wait.

That leaves the movie's own state to a second launch with a small skip
(`:120`, two seconds into playback), or to a re-arm, whichever is
convenient. Two numbers, and the fix has what it needs.

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
| `intro_probe = 1` | records the startup. Per frame: draws counted by the target they land in, logged when that shape changes; every frame of 200 ms or more; every new clear colour, read *before* the black void fix substitutes; and the first frame that draws into an eye texture, called out on its own line. Changes nothing; stands down after three minutes or 96 lines. |
| `census_at_ms = A,B,C` | arms a full draw census at named moments after the session's first frame — up to eight, milliseconds. Each records offscreen draws whatever `census_offscreen` says. Read once, at the first frame. An entry due inside a stall fires when the stall ends. |

Milliseconds and not frames, deliberately: this session measured 1130 fps
through the startup's empty phase and 60 through the movie, so one frame
number is a different moment on every rig and in every session.
`common/timing.h` is that rule, and this is the instrument that most wanted
to break it.

Both count frames the same way `DC` lines do — `intro probe: frame N` and
`DC begin ... frame=N` are the same N.

## Status

Measured 2026-08-28 across two flights. Established: the movie's path, the
freeze, the unreachability of phase A, and — flight 3 — that the composite
covers the whole eye, so the picture's size is decided inside its 1920x1080
source and not by the panel's own geometry. Remaining: where it sits inside
that surface, for the movie and for the splash, which flight 4 reads with a
probe that could not have been aimed at it before today. Plus the
facing-the-wrong-way symptom, which has no measurement of its own yet. No
fix written.
