# The intro movie

Reported 2026-08-28 and **measured the same day**, in one flight, on the field
rig (Frontier launcher install, game build 330683, one eye 5424x5356, Pimax
via OpenComposite). The model this page opened with was a hypothesis built
from the install's own files and from old logs; the flight confirmed it,
including the vertex shader. What follows is the measurement. No fix is
written yet — one number is still missing, and it is named at the end.

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
this flight found for the movie panel, exactly. Same shader, same source
size, same shape of draw. Whether the two differ in their four vertices --
and by how much -- is the one thing flight 3 has to answer, and it answers it
by measuring both rather than by inventing either.

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

**Its placement is in four vertices and nowhere else.** The quad draws from
its **own 80-byte vertex buffer** (stride 20, four vertices — one buffer per
eye, not the shared 4 MB widget pool the loader arc fought), and it binds
**no constant buffer** (census `c=-`). Nothing about where it sits is in a
transform to be substituted; it is those four vertices.

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

## What is still unmeasured

1. **The four vertices.** Their values, their space, and what the other 12
   bytes of the 20-byte vertex hold. This is the only number a placement fix
   needs, and `quad_probe` can now be aimed at it.
2. **Whether the movie is already playing in phase A.** The ten draws a
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

**Flight 3 — the geometry, in BOTH states.**

```ini
[advanced]
quad_probe = 5424x5356:X:6:120
census_at_ms = 4000
intro_probe = 0
```

Two 6-index quads reach each eye and the probe is occurrence-aware, so it
captures all four and logs each one's rectangle plus the bytes past the
position. The movie's is the pair drawn from an **80-byte** vertex buffer at
stride 20; the other pair (`vh=A888D51024D9798E`) is stride 8 out of a 32 KB
buffer and binds a constant buffer. They cannot be confused.

`:120` matters. Without it the capture lands on the first matching frame,
which is frame 2357 — the 2719 ms handover — the single least typical frame
in the session. 120 matching frames is about two seconds at the ~62 fps this
session measured through the movie, so the capture falls in steady playback.
The `4000` census closes open question 2 in the same launch.

**Then re-arm for the splash.** The probe takes one capture per session and
stands down; clearing the spec and setting it again asks for another. Since
the ini is re-read about once a second, that can be done from outside the
game while the player sits at the menu — no headset off, one launch, two
captures: the movie's quad and the splash's, in the same session, on the
same rig, at the same render scale. Those are the two rectangles the fix
needs and the only two.

**Then the fix.** With both rectangles in hand it is a rewrite of four
vertices per eye, and the values written are **the splash's own**, not
anybody's chosen placement — which is what makes the cut from movie to
splash land. `panel_quad.cpp` and `loader_panel.cpp` both already re-issue a
matched draw from rebuilt geometry, and the draw is recognised by shape and
by what it samples (a 6-index quad into an eye texture sampling a 1920x1080
surface whose own source is three R8 planes), not by a game version.

One ordering problem to solve when it is written: **the movie plays before
the splash does**, so the geometry to copy has not been seen yet when it is
first needed. Three ways out, in order of preference, to be decided against
the measurement rather than now — the two quads turn out to differ by
something derivable (a scale the shader or the source size implies); or the
splash's geometry is learned in one session and kept; or the movie's quad is
rebuilt from the same rule the splash's follows, once that rule is read off
two measured rectangles. If they turn out to be identical, the premise is
wrong and this page says so.

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

Measured 2026-08-28 in one flight. The movie's path, the freeze and the
unreachability of phase A are established. The panel's geometry is the
remaining unknown and flight 3 reads it. No fix written.
