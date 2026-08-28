# The intro movie

Reported 2026-08-28. The model below is built from the game's own files and
from field logs that already exist; **nothing has been flown at it, and no
fix is written.** The loading-panel arc next door
(`docs/loading-panel-handoff.md`) spent three architectures and a dozen
flights on a fix built from an unmeasured model whose central conclusion its
own instruments had manufactured. This page is the alternative: state the
model, say which parts are measured and which are inference, and ask for the
one flight that separates them.

## The defect, in the field's words

> When the game first loads in VR, it attempts to play a video at mirrored
> window full resolution, but freezes for 1 or 2 seconds before continuing
> the video in a small, headtrack locked video.

And the wanted behaviour, in the same words:

> load correctly into a black void (the same one that shows up later after
> the video plays or is skipped) and render the video correctly to a panel in
> 3d space (like the splash screen it already renders in the void)

Most players press Escape through the whole thing, which is why this has
never been reported as a bug: it is skipped rather than endured.

Three phases, and a fix has to answer all three.

| phase | what the player gets | what is wanted |
|---|---|---|
| A | the movie at the mirror window's full resolution | a black void with the movie on a panel in it |
| B | a freeze of one to two seconds | no freeze, or the void through it |
| C | the movie continues, small and head-locked | the same panel, still |

## What the movie is (measured, from the install)

Read straight off `Products\elite-dangerous-odyssey-64\Movies\` on the field
rig, 2026-08-28. Every file is Matroska/WebM; the header of each gives the
codec and size.

| file | codec | pixels | length |
|---|---|---|---|
| `Ident_Frontier_EliteNeutral.webm` | VP8 | **1920x1080** | 20.3 s |
| `Ident_Frontier_EliteHorizons.webm` | VP8 | **1920x1080** | 20.5 s |
| `Ident_Frontier_Arena.webm` | VP8 | **1920x1080** | 18.5 s |
| `intro_temp.webm` | VP9 | **1920x1080** | 20.3 s |
| `FrontEnd0/1/A.webm` | VP8 | **1920x1080** | 90-95 s |
| the rest (Salvation, Thargoid, Update 14) | VP8 | **1920x1080** | 63-199 s |

**All of them are 1920x1080**, and the launch idents are about twenty
seconds -- the length the report describes.

They are decoded by DirectShow filters shipped beside the executable:
`webmsplit64.dll` (the splitter), `vp8decoder64.dll` and `vp9decoder64.dll`,
`dsfVorbisDecoder64.dll` for the audio. So the frames arrive on the CPU, and
reach the GPU through an upload -- `UpdateSubresource` or a mapped dynamic
texture. Both are things a census records (`UpdateSubresource` is its 'U'
line, and the Map/Unmap tee is already built), which matters: the movie's
image will not appear as a draw's output anywhere.

`AppConfig.xml` on this rig sets the desktop window to **1280x720**, which
is the "mirrored window full resolution" the report names.

## What the startup does (measured, from field logs)

From the 2026-08-27 17:42 session (Steam install, build 330683, one eye
4848x4788). Nothing in that session was aimed at this; these are the lines
it happened to write.

| time | +s | event |
|---|---|---|
| 17:42:32.738 | 0.0 | the d3d11 proxy attaches |
| 17:42:33.683 | 0.9 | the D3D11 device is created |
| 17:42:33.780 | 1.0 | the Present hook is installed -- frames countable from here |
| 17:42:35.898 | 3.2 | the openvr proxy attaches |
| 17:42:35.901 | 3.2 | `GetRecommendedRenderTargetSize -> 4848x4788 per eye` |
| 17:42:37.353 | 4.6 | `IVRCompositor_014` is asked for |
| 17:42:37.394 | 4.7 | the first eye-sized draw: an `N` of 3 vertices sampling 4848x4788 |
| 17:42:37.399 | 4.7 | **an eye-sized draw, `X` with 6 indices, sampling a 1920x1080 texture** |
| 17:42:38.271 | 5.5 | the compositor hook validates -- both eyes have been submitted |
| 17:42:38.347 | 5.6 | the openvr half tells the d3d11 half what one eye IS |
| 17:42:53.742 | 21.0 | 5425 frames in 20047 ms -- **271 fps** through the startup |

Three things fall out of that table.

**There is a 3.7-second stretch (0.9 to 4.6) in which the game renders and
the compositor has not been asked for.** Whatever is on the monitor then, the
headset is not being handed it by Elite.

**For that whole stretch this DLL does not know what an eye texture is.** One
eye's size arrives from the openvr half at +5.6. Before it, every draw is
"offscreen" as far as the d3d11 half is concerned, and every fix here that
sits behind the eye-texture gate -- `fix.black_void` included -- is inert by
construction.

**271 fps.** A hand on a key lands hundreds of frames from where it meant to.
Phases A and B cannot be caught by hand, and that is why the instruments
below had to be built.

## The 6-index quad, and what it probably is

The `+4.7 s` line above is the interesting one, and it is not a one-off: the
same line, in the same position, appears in **every** field log to hand where
the on-foot resolution had been raised --

```
vScreen: an eye-sized draw sampled 1920x1080, which is not the panel
(5120x2880), so it was left alone. The draw was X with 6 vertices or indices.
```

-- at 11:08:58, 11:21:12, 11:33:54, 17:38:49, 17:03:21 and 17:42:37, always
about five milliseconds after the first eye-sized draw of the session.

That shape is already documented next door. `docs/menu-backdrop.md` measured
the front end's chain:

```
@130  1920x1080 BC1 (the menu still, static)
  |   4-vertex trianglestrip, no depth
  +-> @129  1920x1080 RGBA
  +-> @141  1920x1080 RGBA
        |   6-index quad, VS EF103A7CB4A8369A
        +-> the two eye targets
```

**The hypothesis, stated as one:** the front end -- movie and menu still
alike -- is composed into a 1920x1080 surface, and that surface is put in
front of each eye by a 6-index quad. During the intro the surface holds a
decoded WebM frame; at the menu it holds the BC1 still. Under that
hypothesis the field's "small, head-locked video" IS this quad, and where it
sits is a property of six vertices.

Three things support it and none of them settles it: the movie is 1920x1080,
the front-end surface is measured at 1920x1080, and a 6-index quad samples
something 1920x1080 into each eye at the moment the intro is running. What
would refute it: the quad turning out to sample a menu element that happens
to be that size. The census in flight 2 answers this in one line.

## A coincidence worth testing before anything is built

The panel-distance and curvature fixes recognise the on-foot screen as *an
eye-sized draw whose first sampled texture is exactly the panel's size*
(`srv0IsPanelSized`, `vscreen.cpp`). With `vscreen_res_width/_height` left at
the stock 1920x1080, **the panel size and the front-end surface size are the
same number** -- so the intro's composite is recognised as the panel, and
`fix.panel_distance` and `fix.panel_curvature` should already act on it.

The field logs are consistent with that: in the two sessions run at stock
panel size, 1920x1080 is absent from the "not the panel" list that every
other session carries, which is what "it WAS the panel" looks like from
outside. Neither session had `panel_distance` on, so nothing acted -- the
coincidence has never actually been exercised.

Raising the on-foot resolution -- which the README recommends and which this
rig runs at 5120x2880 -- breaks the coincidence, and the intro composite
stops being recognised by anything.

**Flight 0, before any code.** Set `vscreen_res_width = 1920`,
`vscreen_res_height = 1080`, `fix.panel_distance = 1.5`,
`fix.panel_curvature = 0`, launch, and watch the intro without pressing
Escape. `panel_distance` is live, so it can also be swung during the movie
and watched. If the movie's rectangle moves, the hypothesis above is
confirmed and half the mechanism a fix needs already exists and is field
proven. It costs one launch and no build.

## What is still unmeasured

1. **Whether phase A reaches the headset at all.** Nothing is submitted for
   the first 4.6 seconds, so either the headset is showing SteamVR's own
   void and the full-resolution movie the report describes is the one on the
   monitor -- in which case phase A is not reachable from here, because EDVR
   cannot submit frames the game never made -- or the game composites
   full-view into the eye textures earlier than the log has ever recorded.
2. **What the freeze is.** One long frame or a run of slow ones. A single
   long frame at the +4.6 boundary is VR bring-up; a run is something else.
   Nothing in any log to hand measures the game's frame times before VR
   starts.
3. **The geometry of the 6-index quad.** Its extent, its vertex format, the
   space its positions are in. This is the number a substitution needs and
   there is nothing to guess from.
4. **What the surround is.** `black_void` reports "void cleared to black 0
   time(s)" in every field log here, so in these sessions Elite never asked
   for the flat grey clear that fix was built for. Whether the intro's
   surround is a clear at all, and of what colour, is unrecorded.
5. **Whether EDVR is party to the freeze.** The report describes stock
   behaviour, but it has never been A/B'd. Renaming `d3d11.dll` aside for
   one launch settles it and costs nothing.

## The instruments (both `[advanced]`, off by default, live)

Built 2026-08-28 for this, because nothing here could be aimed at a startup:
the census key is armed by hand at 271 fps, `census_auto` needs a target size
somebody has already censused *and* two quiet seconds that a target drawn
into from frame one never gets, and every draw-path fix sits behind an
eye-texture gate that cannot answer for the first four and a half seconds.

| key | does |
|---|---|
| `intro_probe = 1` | records the startup. Per frame: the draws counted by the target they land in, logged whenever that shape changes; every frame of 200 ms or more; every new clear colour, read *before* the black void fix substitutes; and the first frame that draws into an eye texture, called out on its own line. Changes nothing; stands down after three minutes or 96 lines. |
| `census_at_ms = A,B,C` | arms a full draw census at named moments after the session's first frame -- up to eight, milliseconds. Each records offscreen draws whatever `census_offscreen` says. Read once, at the first frame. |

Milliseconds and not frames, deliberately: a loading screen has been measured
at 1790 fps and a menu at 13, so one frame number is a different moment on
every rig and in every session. `common/timing.h` is that rule, and this is
the instrument that most wanted to break it.

Both count frames the same way `DC` lines do -- `intro probe: frame N` and
`DC begin ... frame=N` are the same N -- so the timeline and the censuses
inside it line up without arithmetic.

## The flight plan

**Flight 0 -- the free one.** The panel-size coincidence above. One launch,
no build, and a yes tells the fix where to start.

**Flight 1 -- the shape of the startup.** `advanced.intro_probe = 1`, launch,
let the movie play to its end without pressing Escape, quit. Send the gfx log
(it flushes on exit -- a live log reads 0 bytes).

What each answer means:

* composition lines with **no starred target**, then a stall line, then
  starred ones: phase A is flat and unreachable from here; the fix covers B
  and C.
* **starred targets before the stall**: phase A is a draw in the eye
  textures, and it has a geometry a fix can reach.
* the stall line names the freeze -- how long, and where it sits relative to
  the handover.
* the clear lines say whether the intro's surround is a clear, and of what
  colour. That decides whether "a black void" is a gate to widen or a draw to
  suppress.

**Flight 2 -- the movie's draws.** With flight 1's timeline in hand, set
`advanced.census_at_ms` to two or three moments picked off it -- one inside
phase A, one just after the stall, one well into phase C -- with
`advanced.census_frames = 3`. `tools/diff_draw_census.py` diffs any two. The
draw that carries the movie is the one whose sampled texture is rewritten
every frame while its geometry does not move, and the 6-index quad above is
either on that line or it is not.

**Flight 3 -- the geometry.** `advanced.quad_probe` aimed at the signature
flight 2 names, which reads the quad's rectangle out of the vertex buffer.

Only then is there a fix to write.

## What a fix could look like

Sketched so flight 1 is read with the right questions in mind. Every line is
contingent on the measurement.

**The panel.** If the 6-index composite is the movie, placing it is the same
problem the on-foot screen already solved: `panel_quad.cpp` and
`panel_curve.cpp` rebuild that composite's geometry, `loader_panel.cpp`
re-issues a matched draw from remapped vertices, and `srv0IsPanelSized` is
one size comparison away from recognising the front-end surface as well as
the panel. Head-locked or world-locked then becomes a choice rather than a
limitation.

**The void.** If the intro's surround is a flat clear of an eye-sized target,
`fix.black_void` already knows what to do and has simply never been reaching
this early. If it is drawn rather than cleared, it is a draw to name first.

**The freeze** is the game bringing VR up, and nothing here makes that
faster. What can change is what is on screen while it happens -- and only if
phase A turns out to be reachable at all.

**Not on the table: skipping the movie.** It is the game's content, the field
asked to see it properly rather than to lose it, and a patch that presses
Escape on somebody's behalf is a patch deciding what they get to watch.

## Status

Model written, instruments built and building clean, nothing flown. Both keys
are off in the shipped `edvr.ini` and free when off.
