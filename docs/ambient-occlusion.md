# Ambient occlusion that disagrees between the eyes: a design for the hunt

*A design document, written before any capture. Written 2026-09-07 on
branch `claude/asteroid-ao-inconsistency-xt19n7` off main `dc3ebad`. Claims
about EDVR cite the source; claims about the game are labelled measured
(this repo's censuses, dumps and disassemblies), read (taken from a
captured shader's bytecode), or believed; what only a live session can
settle is collected under Phase 0. Nothing here is built. Two captures have
arrived on issue #23, and their readings are under
[What the first worksheet said](#what-the-first-worksheet-said),
[What the capture said](#what-the-capture-said) and
[What the second capture said](#what-the-second-capture-said). **The pass
is named**: Elite's ambient occlusion is HBAO, a chain of three compute
dispatches per eye per frame -- `FB277B33F0865348`, `9347F8FC2DCE0248`,
`D31E7812990B19A6` -- and mechanism C is measured, its sixteen-layer
deinterleave anchored to the pixel grid. Mechanism A is the one thing left
open, and no shipped instrument can reach it.*

## The ask

Field-reported 2026-09-07, verbatim:

> Ambient occlusion on asteroids is inconsistent between eyes. pls make it
> consistent :> it's most visible in icy belts. look at any asteroid with
> detailed cracks. I also have ambient occlusion on high/ultra/whatever the
> highest setting is

It came with a screenshot: the mirror view from a Gutamaya cockpit parked
in an icy ring, one large cracked fragment filling the canopy, a prospector
limpet controller selected. The mirror shows one eye, so it cannot show the difference,
and nothing sent so far can. What the report does fix is the setting (the
top level), the scene (an icy ring, close to a large fragment with cracks),
and that the difference is steady enough to look at rather than a flicker
caught once.

"Inconsistent" has two readings, and they are the first thing to ask for
because they point at different mechanisms:

- **A level difference.** The cracks are darker, or wider, in one eye than
  the other. A shading that one eye has and the other has less of.
- **A pattern difference.** Both eyes have the shading, but its grain -- the
  speckle screen-space occlusion leaves after its blur -- differs, so the
  cracks sparkle or crawl between the eyes. Binocular rivalry on a texture.

The class of fault is known to players and unfixed: the standing advice on
the Frontier forums for VR is to turn ambient occlusion off, with the
pilot's arm named as where its wrongness shows
([forums.frontier.co.uk, "What are the best graphics settings in VR?"](https://forums.frontier.co.uk/threads/what-are-the-best-graphics-settings-in-vr.583899/)).
No entry for it was found on Frontier's public tracker (believed: one
search, 2026-09-07). This repo's is
[#23](https://github.com/characterecho-sean/edvr-unofficial-patch/issues/23),
filed from the worksheet the same day.

The asteroid is where this reporter sees it; on none of the readings
below is it where the fault lives. Screen-space occlusion is one chain over
the whole frame (read, from the game's own symbols: five compute entry
points from a linear depth to a blurred result), so whatever the mechanism
is, it is the same on the pilot's arm the forum thread names, in a
station's hangar, and on a planet's surface -- the cracks of an icy
fragment are simply where the most silhouettes per degree sit in view,
close enough for the eyes to disagree about them. The capture asks where
else it shows for that reason, and a fix to the pass fixes it everywhere
the pass runs. The section [Beyond asteroids](#beyond-asteroids) says
what the other scenes add.

## The short version

Screen-space ambient occlusion is computed from each eye's own depth
buffer, so by construction the two eyes' occlusion differs wherever their
views do -- at every silhouette, and cracks are nothing but silhouettes.
The goal is therefore the one the exposure fix set
([eye-brightness.md](eye-brightness.md)): not two identical images, but two
eyes that agree about the same surface. What can be made to agree is
everything the game feeds the pass that is not the eye's own view: its
random seed, its noise pattern, and whether the pass ran for that eye at
all this frame.

Three mechanisms fit "cracks disagree, worst at the top setting". Each has
a signature that one capture shows and a fix shape with a precedent in this
tree, and they are laid out in that order below: cheapest fix first.

| | Mechanism | Signature in the capture | Fix shape | Precedent |
|---|---|---|---|---|
| A | the kernel's rotation seed steps per pass, not per frame | eye A's and eye B's constant dumps differ in one stepping field | substitute eye A's constants into eye B's pass | `billboard_fix.h`, the panel-distance discipline; `dispatch_cb1_lend` |
| B | the pass runs for one eye per frame, alternating | the pass's lines appear once a frame, eyes alternating | re-issue the pass for the missing eye with its own inputs | `scanner_body`, the lend, reversed |
| C | the noise is anchored to the pixel grid (and the buffer may be half size) | constants identical, both eyes every frame, the occlusion target differs by an uncorrelated grain | a transcribed replacement shader whose noise is hashed from world position | `shader_swap.h`; the sun-glare and particle transcriptions |

And a fourth to rule out before any of them: EDVR itself, meaning the
temporal pass. One session on the stock game settles it.

The order of work: **Phase 0**, one session on the reporter's rig with the
shipped instruments and nothing new built; **Phase 1**, reading it at the
desk; **Phase 2**, one live probe per remaining question, each a key that
already exists or a small instrument named below; then the fix behind a
default-off key, and default on only if it never engages on a rig without
the fault, which is `scanner_body`'s argument for its own default.

## What the tree already knows

Measured, with the source:

- **Nothing in EDVR touches ambient occlusion.** A grep for "occlusion"
  finds occlusion *queries* (`src/d3d11/draw_census.cpp:1106`, the DCL
  query brackets) and the sun glare's own occlusion test
  (`sunglare_vs.h:300`) and nothing else. The pass has no name, no hash, no known slot.
- ~~**The eye-split dump has probably already photographed the buffer.**~~
  **Wrong, and the first capture proved it wrong** -- kept here because it
  is the guess this hunt paid for. The reasoning was: the measured field
  frame carried sixteen eye-sized targets, among them a 2324x2392 target at
  format 60, `R8_TYPELESS`, one byte a texel (`src/d3d11/eye_split.cpp:65`;
  [eye-split.md](eye-split.md)); a one-channel eye-sized target is the
  shape of an occlusion buffer. It is the shape of a sun-shadow mask too,
  and that is what it turned out to be. The occlusion is written by compute
  shaders into UAVs, which the dump cannot see at all. See
  [What the capture said](#what-the-capture-said), Q4.
- **This engine runs some passes for one eye per frame.** The scanner-body
  hunt recorded the four atmosphere draws vanishing from one eye on
  alternate frames in healthy normal flight, and filed it as ordinary
  engine behaviour ([scanner-body.md](scanner-body.md), "three patterns
  that looked like the fault"). Mechanism B is that pattern applied to a
  pass nobody has looked at.
- **EDVR's own jitter cannot split the eyes.** The temporal pass's
  sub-pixel shift is added to both eyes' tangents from one `jitDx`, `jitDy`
  pair (`src/openvr/system_hook.cpp:469-474`), and only while both eyes'
  projection formula checks passed (`system_hook.cpp:1466`). It moves both
  frusta together or neither. The cull guard, off by default, widens both
  eyes' frusta by the same fractions, and the supersample resolve runs one
  kernel per eye at submit. The
  temporal pass's per-eye *history* is the only EDVR mechanism that could
  make the eyes converge to different pictures, and only while it is on.
- **The game's view rows are readable.** The frame's true view matrix sits
  at float 932 of the scene block (`src/d3d11/temporal_pass.cpp:2207-2213`,
  measured by the sun-glare two-shot dump), and the glare train's constants
  carry the camera rows per draw (`billboard_fix.h`). A replacement shader
  that needs to know where the eye is has somewhere to get it.
- **Every fix shape below has shipped once already.** Copying a per-eye
  resource from the first eye to the second (`exposure_fix.h`); lending
  one draw a binding the other eye used (`scanner_body`,
  [scanner-body.md](scanner-body.md)); substituting a copy of a draw's
  constants around that one draw and restoring the game's buffer after
  (`billboard_fix.h`, the panel-distance discipline); substituting a
  texture slot with a 1x1 uniform (`hud_grain.h`, `holo_fix.h`);
  compiling a replacement vertex, pixel or compute shader at runtime that
  stands down on any failure (`shader_swap.h`); and, as live instruments,
  equalising a compute pair's output (`experimental.dispatch_pair_sync`)
  or its `b1` (`dispatch_cb1_lend` / `_strip`).
- **Every instrument the hunt needs exists.** The census with its draw,
  offscreen, dispatch, copy, clear and constant-watch lines
  (`draw_census.h`); its differ (`tools/diff_draw_census.py`); the
  eye-split dump and its registered differ; the skip probes by shader hash
  for draws and dispatches; the shader dump. All off by default and free
  when off.

Believed, and each has a gate in Phase 0:

- Elite's ambient occlusion is a screen-space technique that reads the
  eye's depth (and possibly its normals), rotates a sample kernel per pixel
  by a noise value, writes a one-channel buffer, and blurs it before the
  lighting resolve reads it. The menu's levels change the sample count, the
  radius, or the resolution it runs at. The per-level parameters are in the
  game's `GraphicsConfiguration.xml`, which Save logs already bundles
  (`src/installer/logbundle.cpp:414`); read the file rather than this
  sentence.
- The pass runs once per eye, on the eye's own depth. If it does not --
  if it runs once and both eyes read it -- the report is measuring
  parallax against a mono occlusion, which is mechanism C's ceiling case
  and needs the same capture to see.

## The three mechanisms

### A. The seed steps per pass

Screen-space occlusion rotates a small sample kernel per pixel, and most
implementations add a per-frame offset to that rotation so a temporal
filter, or simply the eye, averages the pattern over time. If the offset
advances per *pass* rather than per *frame*, the second eye samples the
kernel at the next frame's rotation: the same crack is occluded by a
different set of samples in each eye, the blur that follows never quite
removes the difference, and the result is a pattern difference stable in
kind. This is the idiom the FSS hunt built `census_cb_watch` for -- a
value "stepping per WRITE rather than per FRAME" (`edvr.ini`, the
`census_cb_watch` block) -- and it would show the same way here.

**Signature.** With `census_cb_watch` naming the pass's shader, each frame
of the census carries a DCW dump for eye A's pass and one for eye B's,
paired by `q=`. If one field differs between them, and the difference from
B to the next frame's A equals the difference from A to B, that field is
the seed and it steps per pass.

**Fix.** For eye B's pass, bind a copy of the constants in which that
field carries eye A's value, and restore the game's buffer after the draw
-- `billboardBegin`/`billboardEnd` around a matched draw with a shadowed
write, the mechanism that steadied the sun's flare and that the
panel-distance fix established (`billboard_fix.h`). For a compute pass the
same substitution wraps the dispatch, the way `dispatch_cb1_lend` wraps
one today. If the seed turns out to live in a resource rather than a
constant -- a frame counter written by an earlier dispatch -- the exposure
fix's copy is the shape instead (`exposure_fix.h`). Direction is the
exposure fix's too: the first eye rendered is the reference, and the
second is made to follow it. One substitution per frame; free when off.

### B. The pass runs for one eye per frame

The parity the scanner hunt recorded, on a pass nobody has named. An
occlusion computed on alternate frames per eye -- or its blur, or the
temporal half of it if the top level has one -- leaves one eye's
occlusion a frame stale. A headset's head never stops moving
([anti-aliasing.md](anti-aliasing.md)), so the stale eye's occlusion sits
a fraction of a pixel to a pixel off its geometry, and the features that
show it are the thin dark ones: cracks. This reads as a *level*
difference that changes with head motion, and as agreement while the head
is truly still.

**Signature.** In the census, the pass's family of lines appears once per
frame with the eyes alternating (pair against the two eyes' depth clears,
the DCL `D` lines that open each eye's pass), or twice per frame with
one eye's lighting resolve reading a target the pass last wrote in the
previous frame -- the `r=` token of the pass against the `s=` tokens of
the resolve, frame by frame.

**Fix.** Re-issue the pass for the missing eye every frame, with that
eye's own inputs: its depth in the slot the census shows (`s0`, or `d=`),
its own target, its own constants. That is the scanner-body lend turned
around -- lend the eye the pass rather than the draw a binding -- and it
costs the pass's GPU time once more per frame, which at the top level is
the one price on this page a player might notice. It carries the largest
correctness risk of the three, because the game's later passes were
written expecting the staleness, and it is not to be built before Q1 to
Q3 are measured.

### C. The noise is anchored to the pixel grid

With identical constants and the pass running for both eyes every frame,
the per-pixel rotation still comes from somewhere, and the usual somewhere
is a small tiling noise texture indexed by screen position -- `hud_grain`
found exactly such a table, 256x256 at slot 1, behind the flight HUD's
shimmer (`hud_grain.h`). The same surface point then gets a different
rotation in each eye because it lands on a different pixel: content at
infinity sits about 365 pixels apart between the eyes at a 2517-pixel
eye width ([eye-split.md](eye-split.md)), and nearer content further,
per pixel, with the eye's own view. The blur is screen-space too. And if the
top level runs the pass at half resolution with a depth-aware upsample
(believed; the target's size in the census's interned table settles it),
thin depth discontinuities -- cracks again -- resolve differently in each
eye. None of this is a bug in the engine's own terms. It is a technique
that assumes one viewpoint, run for two.

**Signature.** A and B ruled out by their own gates, and the eye-split
diff of the one-channel target showing a fine, zero-balance difference:
parallax moves content sideways with brightness conserved, and so does
this, but with the *texture* uncorrelated between the eyes at the
registered far field and along the cracks. The reporter's own reading --
pattern, not level -- is the same evidence from the other end.

**Probe, cheap and decisive.** Put a 1x1 uniform texture in the noise
table's slot for the pass's draws, `hud_grain`'s mechanism with the shader
hash and the slot as parameters instead of its own. Every pixel then uses
one rotation: the occlusion goes banded, visibly and unpleasantly, and the
two eyes' patterns become identical in kind. If the reporter's
inconsistency disappears under the banding, C is the mechanism and the
noise is the lever. This is the one instrument this document proposes
building before any fix, and it is `hud_grain.cpp` with a key.

**Fix, C1: the transcription.** A replacement shader for the pass
(`shader_swap.h`: vertex, pixel or compute, compiled at runtime, standing
down to the game's own on any failure), identical to the game's
disassembly except that the rotation is hashed from a world-space position
reconstructed from the pixel's depth, so one surface point gets one
rotation in both eyes. The pattern then also holds still on the surface
under head motion instead of crawling across it, which the temporal pass
would prefer as well. It needs the eye's inverse view per frame -- EDVR
reads the view rows (`temporal_pass.cpp:2207`) and the per-eye camera rows
through the glare tee (`billboard_fix.h`) -- handed to the shader in a
constant buffer at a slot the census shows free. The sun-glare and
particle fixes are transcriptions of exactly this kind, and their lesson
holds: transcribe the disassembly, never a likeness of it, because the
witchspace starfield was given the flare's replacement on a likeness and
vanished (`edvr.ini`, the `census_skip` block).

**Fix, C2: the resolution.** If the pass runs at half size, nothing
outside the game restores the detail short of re-issuing it at eye size
into an EDVR-owned target and substituting that for the game's -- B's
machinery carrying C1's shader. Listed as the ceiling so nobody promises
it lightly; it is not the first thing to build.

### D. EDVR's own doing

To be ruled out first, because it costs one session and no analysis:

- **The temporal pass.** Per-eye history, blended every frame. A noise
  that is random per frame converges under it to a mean, and if anything
  above makes the two eyes' noise differ, the histories converge to
  different means and the pass makes a flicker into a steady disagreement.
  `temporal_aa` is off by default and the report does not say whether it
  is on. Control: `temporal_aa = off`, then the stock game with
  `d3d11.dll` renamed aside for one session.
- **Not the jitter, not the resolve, not the guard**, for the reasons
  measured above.

## Beyond asteroids

The reported scene is the hardest one to reproduce (a ring, a large
fragment, a parked ship) and not the most telling one. Three others are
worth a look from anyone who has the fault, and each adds something the
asteroid cannot:

- **The pilot's arm and the cockpit.** The nearest geometry in the game,
  where the two eyes' views differ most and a stereo-blind occlusion is at
  its worst; and where a stale eye (mechanism B) would show as a level
  difference that swings with every head movement, because near content
  moves the most pixels per degree. The forum thread names it; the
  worksheet asks for it.
- **A station's hangar.** Edges everywhere, controlled lighting, and no
  ring to find -- anyone can dock. If a second capture is ever needed,
  this is the scene to take it in, because every rig can reach it in two
  minutes and the census diff is cleanest where the lighting does not
  change between the two presses.
- **A planet's surface.** Terrain has its own passes (the cull guard's
  whole subject), and whether the occlusion runs over terrain the same
  way, or at all, is one more line of the census.

What the breadth decides: if the fault shows everywhere the pass runs, it
is one of the three mechanisms above and the fix is at the pass. If it
shows only on asteroids, something asteroid-specific feeds the pass
differently per eye -- a material or a detail map that is not the
occlusion's own doing -- and the hunt moves to the asteroid's draws, with
the same instruments. The reporter's answer to "where else" in Part 2 of
the worksheet is the first evidence, and Q11 below is the gate.

## Phase 0: the capture

One session on the reporter's rig, shipped instruments only. The restart
is for the shader dump, which records only shaders created while it is
on; everything else is live.

```
[hotkey]
dump_draws = NUMLOCK

[fix]
temporal_aa = off

[advanced]
census_offscreen = 1
census_frames = 2
census_lines = 16384
glare_shader_dump = 1
```

`census_offscreen` is not optional here: the occlusion buffer is not the
eye texture, and a pass that runs at half size lands on a DCO line and
nowhere else. Two frames at sixteen thousand lines is the budget a full
scene with offscreen draws needs: a full scene is thousands of draws a
frame, and three frames of that spend the default cap on the way past
(the `census_offscreen` block in `edvr.ini`).

1. Set ambient occlusion **Off** in the game's graphics options. Fly into
   an icy ring and park a few hundred metres from a large fragment with
   visible cracks. Hold still. Press the census key once: this is the
   baseline, the frame without the pass.
2. Set ambient occlusion to its highest, return to the same view, and
   press the census key again. If the game insists on a restart for the
   change, do this as a second session; the differ takes two logs.
3. With ambient occlusion still at its highest, in `edvr.ini` set
   `eye_split = 3` under `[advanced]` and save, with the fragment in
   view. One hitch, one dump. The key is live.
4. Quit. Run the installer's **Save logs**. Then zip `edvr_logs\dumps`
   and `edvr_logs\shaders` by hand -- Save logs collects the logs, the
   breadcrumbs, the settings and the game's graphics files, not the
   dumps or the shaders (`logbundle.cpp:384-453`).

And in prose, the answers only the person in the headset has:

- Level or pattern -- are the cracks *darker* in one eye, or *grainier*?
  Does it change when the head is truly still?
- Which eye is worse, if either.
- Whether it reproduces on the stock game, with `d3d11.dll` renamed aside
  for one session.
- Whether it reproduces at ambient occlusion **Low**, and whether Low
  looks like a different technique or the same one with less of it.
- Headset, runtime, and Elite's HMD Quality. A half-resolution pass under
  a high render scale is a different picture from one at the native size.

## What the first worksheet said

Issue [#23](https://github.com/characterecho-sean/edvr-unofficial-patch/issues/23),
2026-09-07, filed by the reporter from the worksheet with the Save logs
bundle and the dumps attached. The rig: a Quest 3 through OpenComposite on
SteamVR's OpenXR layer, HMD Quality 1.0, the supersampling slider at 1.0,
an RTX 4080 Super, EDVR 0.14.0, no other mods, temporal AA normally DLAA.
Ambient occlusion at Ultra. The prose half of Phase 0, and what each
answer closes:

| Answer | What it says |
|---|---|
| Still there on the stock game, EDVR's file renamed aside | **D is closed.** The fault is the game's, like the black planet and the FSS split before it, and EDVR can only reach it from outside. |
| Still there with temporal AA off | The pass is not involved, and DLAA's history was not shaping it. |
| Unchanged with the head held still | **B is closed.** A stale eye equals the current one when nothing moves, so an alternate-frame pass would agree at rest. It also rules out any temporal half of the pass. |
| Level, not pattern: the cracks are darker or wider in one eye | The difference survives the game's blur as an amount, not as grain. "Wider" is worth keeping: a half-size buffer brought back up through a depth-aware filter makes crack edges a different width in each eye (C2). |
| Neither eye consistently worse: they just disagree | The darker eye changes crack by crack. A missing or weaker pass makes one eye consistently lighter; a view-dependent estimate flips sign from one crack to the next. This is C's shape, and A's if the seed differs per eye, since a different kernel rotation is a different estimate at every pixel. |
| Same at any distance | Not a near-field effect. Whatever it is happens at the pass, not in what feeds it. |
| Low looks exactly the same as Ultra, and shows it too | The levels change the amount of work, not the technique. The fault is in the technique's stereo-blind core, not in a quality tier's extra pass. |
| Only icy rings so far | Q11 stays open. The arm and the hangar have not been looked at. |

What the prose leaves: **A or C**, and the census and the dump decide
between them (Phase 1). The reporter did every step of the session and
felt the hitch, so the bundle should carry two censuses, one eye-split
dump and the shader dump.

## What the capture said

Read at the desk 2026-09-07, from the two zips on the issue: the Save logs
bundle and the hand-made zip of `edvr_logs\dumps` and `edvr_logs\shaders`.
Nothing from either is in this tree. The session log names EDVR v0.14.0
and game build 332841, and the eye is 2528x2704 as `openvr_api.dll`
published it.

The plan expected the census to name the pass and the other files to
describe it. It came out the other way round: **the census did not record
at all**, and the game's own files named the pass outright.

### The census did not record

Both presses were thrown away, and the log says so in the reporter's own
session:

```
[12:20:26.828] the draw census key was pressed, but another window had focus, so nothing was captured.
[12:20:59.848] the draw census key was pressed, but another window had focus, so nothing was captured.
```

No `DC begin census` line exists anywhere in the log, and the breadcrumbs
carry none either. The timing fits the worksheet exactly -- the two presses
are thirty-three seconds apart and the eye-split dump lands at 12:21:39 --
so the reporter did everything asked; the instrument dropped it.

The cause is measured, not guessed. `dump_draws` is one of EDVR's own keys
rather than a binding adopted from Elite, so it keeps the foreground check
(`hotkey.cpp:148-149`, `gameHasFocus` at `hotkey.cpp:59-65`), and only the
camera and FSS keys are marked game-mirrored (`device_hook.cpp:1266-1270`).
In VR the foreground window is ordinarily the compositor's mirror or the
settings app and not Elite's, which `hotkey.h:66-67` already says in as
many words. **This is a design fault in the instrument, not in the
reporter**: the census key is the one key a player presses while wearing a
headset, and it is the one key that requires the flat window to be
focused. The clock-driven arming below is the way round it, and making the
census key game-mirrored -- or exempting it -- is the second instrument
change this hunt asks for, after Q4's.

Without a census, Phase 1 steps 1, 2 and 6 could not be done, and with them
every Phase 2 probe, each of which takes a shader hash as its only
argument. Steps 3, 4 and 5 were attempted from the other files, and two of
the three came back with something the plan had not expected.

### Q9, and the pass's name: it is HBAO, and it is compute

Read, from `GraphicsConfiguration.xml` in the bundle (lines 75-137) and
from the game executable of the same build:

Elite's ambient occlusion is **HBAO** -- horizon-based, a `<HBAO>` block with
four tiers, `Off`, `Low`, `Medium`, `High`. Across the three enabled tiers
exactly three values change, and nothing else:

| | Low | Medium | High |
|---|---|---|---|
| `HBAO2_Bias` | 0.3 | 0.2 | 0.05 |
| `HBAO2_BlurSharpness` | 8.0 | 256.0 | 256.0 |
| `ResolutionScale` | 0.25 | 0.5 | 1.0 |

Everything else is common to all three: `HBAO_RadiusInMeters` 3.0,
`HBAO_NearRadiusInMeters` 2.0, `HBAO_NearDistance` 30.0,
`HBAO_PowExponent` 5.0, `HBAO_Bias` 0.5, `HBAO_BlurSharpness` 0.5,
`HBAO2_ScreenRadius` 0.07, `HBAO2_PrescaleWithClamp` 1.0,
`HBAO2_PowExponent` 1.0, `HBAO2_Strength` 4.0, `HBAO2_PrenormalFade` 0.8,
`MultiSampleCount` 1. The tier is chosen by `<AOQuality>` in the profile,
and the reporter's `Custom.4.4.fxcfg` carries `<AOQuality>3</AOQuality>` --
the fourth entry, `High`, at full resolution. The index-into-the-block
reading is cross-checked against five other keys in the same file whose
blocks have different lengths (`EnvmapQuality` 1 of 2, `GalaxyMapQuality`
2 of 3, `BloomQuality` 0 of 4, `TerrainQuality` 4 of 5, `VolumetricsQuality`
3 of 4) and holds for all of them. The menu's top entry is `High`; the
worksheet's "Ultra" is the same tier.

Two consequences, immediately:

- **C2 is closed.** At the reported setting the pass runs at
  `ResolutionScale` 1.0 -- full eye resolution -- so there is no
  half-resolution buffer to restore. And the fault is unchanged at `Low`,
  which is quarter resolution with a blur sharpness of 8 rather than 256,
  so it is not a resolution artefact at any tier.
- **The worksheet's "Low looks exactly the same as High" is now odd
  enough to re-ask.** A quarter-resolution pass under a much softer blur
  should not look identical to a full-resolution one. The likeliest
  explanation is that the tier did not take without a restart, which would
  mean control A of the worksheet measured the top tier twice. Worth one
  line in a reply, not a mechanism.

The executable settles what the census would have: the AO **is a chain of
compute shaders**. Its entry-point names and its HLSL binding names are in
the image in clear (verified here on a local install of the same build,
332841, and `GFSDK` appears zero times, so this is Frontier's own port and
not NVIDIA's linked library):

```
HBAO_CONSTRUCT_LINEAR_DEPTH_ONLY_CS   HBAO_CONSTRUCT_NORMAL_AND_DEPTH_CS
HBAO_PERFORM_AO_CS                    HBAO_REINTERLEAVE_AND_BLUR_CS
HBAO_REINTERLEAVE_BLUR_UPSAMPLE_CS
g_constructNormalAndDepthArraySourceLinearDepthTexture
g_constructNormalAndDepthArrayOutputDepthTexture
g_performAONormalSourceTexture   g_performAOSourceDepthTextureArray
g_performAOData                  g_performAOOutputTexture
g_reinterleaveAndBlurAO          g_reinterleaveAndBlurOutput
>fRenderHBAONode::PerformAOData  c_flagsTexture  OutputHBAO
>HBAO_Jitter                     fFragment.HBAO.2D
```

and, in one contiguous run beside them, the shader's whole parameter list:

```
c_viewDepthTexture  c_randomTexture  c_quarterResolution  c_fullResolution
c_invQuarterResolution  c_invFullResolution  c_UVToViewA  c_UVToViewB
c_radiusToScreen  c_negInvR2  c_nDotVBias  c_AOMultiplier  c_powExponent
c_blurSharpness  c_AODepthTexture  c_deinterleavedTexturing
c_useJitterTexture  c_enableBlur  c_jitterPositionScaler  c_jitter
c_float2Offset  c_fullResNormalTexture  c_quarterResDepthTexture
c_AOTextureArray  c_unpackZ  c_viewMatrixT  c_nearRadiusToScreen
c_nearNegInvR2  c_nearDistance
```

with `SeparateDepthTextureInto8Targets`, `ReconstructNormalPS`,
`ReinterleaveAOPS` and `DebugNormalsPS` immediately before them -- the
pixel-shader half of the same chain, kept alongside the compute one.

That list is the answer to **Q6, in kind**. The pass takes its per-pixel
kernel rotation from a jitter that is *selected by where the pixel is*:
`c_deinterleavedTexturing` with `c_float2Offset` splits the depth buffer
into sixteen layers by the pixel's position modulo four
(`g_performAOSourceDepthTextureArray`, `c_AOTextureArray`,
`c_quarterResDepthTexture`), runs the horizon sweep per layer with that
layer's own `c_jitter`, and reinterleaves; the alternative path,
`c_useJitterTexture` with `c_randomTexture`, samples a small tiling table
at the same pixel grid. Either way the rotation is a function of the pixel
coordinate and of nothing else. **That is mechanism C's premise, no longer
believed but read from the game's own symbols**: the same surface point
lands on a different pixel in each eye, so it falls in a different layer,
so it is estimated with a different rotation, and cracks are where that
shows because cracks are all silhouette.

It is also **the answer to Q2: a dispatch, not a draw** -- which changes
which probes apply (`census_skip_dispatch`, `dispatch_pair_sync`,
`census_cb_watch` reading a compute shader's b0 and b1), and which is why
the shader dump was always going to come up empty. See below.

### Q4: the dump did not catch the occlusion, and could not

The eye-split dump fired cleanly -- 24 targets, both eyes at every stage of
one frame -- and **none of them is the occlusion buffer.** The reason is
structural, and the doc half-anticipated it at Q4: the dump records the
render target bound at slot 0 of a *draw*
(`OMGetRenderTargets(1, ...)`, `eye_split.cpp:244-250`, called from
`vscreen.cpp:2053`), and every stage of Elite's HBAO writes a compute
shader's UAV -- `g_performAOOutputTexture`, `g_reinterleaveAndBlurOutput`.
A UAV that is never bound as a render target is invisible to this
instrument. So is a dispatch.

It is worth saying what the dump *did* catch in that shape, because it is
the thing a hasty reading would call the occlusion. One target is
one-channel and eye-sized, in both eyes:

```
EYESPLIT stage=5 shape=632x676f60 eye=0 draws=2 src=2528x2704 step=4 pitch=2560
EYESPLIT stage=8 shape=632x676f60 eye=1 draws=2 src=2528x2704 step=4 pitch=2560
```

Format 60 is `R8_TYPELESS`, one byte a texel (`eye_split.cpp:63-73`), at
the full eye size, two draws into each eye's copy, sitting between the
linearised depth and the 53-draw lit scene. Measured from the file against
the same eye's linear depth, it is exactly 255 on 100.0% of the texels at
the far plane, continuous on geometry (55% at 0, 32% at 255, 13% between),
saturated to 0 across whole asteroids at 300 to 3000 units, back at 255
beyond about 10^5, and on the near cockpit it draws hairlines along the
canopy sill and the console rim.

**It is the sun-shadow mask, not the occlusion**, and the reporter's own
shader dump proves it. `ps_7EAC71963E66C5FE` is a full-screen deferred
shadow resolve. It loads the linearised depth at integer pixel coordinates
-- the same `R32_TYPELESS` target `ps_CB95394B50D737D6` filled -- builds a
view ray from `cb2[1]`, `cb2[2]`, `cb2[4]` and rescales it to that depth,
transforms into light space by `cb2[7..10]`, reads a flag byte
(`* 255`, `ftou`, then `and l(4)` and `and l(192)`) where bit 2 forces full
shadow and bits 6 and 7 divert to a precomputed screen-space term, and
loops over `cb2[35].x` cascades, each a scale and bias at `cb2[i + 11]` /
`cb2[i + 19]` with a smoothstep crossfade between the two it lands in. The
filter is a cubic B-spline over sixteen `gather4` fetches, and the shadow
map is a **moment** shadow map: the de-quantisation matrix at lines 223-227
(`-0.035956`, then rows beginning `0.222774`, `0.154968`, `0.145199`,
`0.163127`) is Peters and Klein's, verbatim, followed by the Hamburger
4MSM Cholesky solve and a light-bleeding reduction.

The line that settles it is the fallback: **when no cascade contains the
pixel the shader moves `l(1.000000)`** and both crossfade weights go to
zero, so the result collapses to the flag byte's default, which is 1.0.
It ends `mad o0.xyzw, r3.xxxx, r0.xxxx, r0.zzzz` -- one scalar splatted to
four channels, which is what an `R8` render target takes the `.x` of. Every
measured property above follows: exactly 1.0 on the sky, 1.0 again past the
last cascade, and hairline self-shadowing at creases on near geometry where
the cascade resolution is highest. Even the histogram is the shader's own
arithmetic: its tail is `movc r0.x, r0.z, r0.x, l(1.000000)` followed by a
light-bleeding remap, `saturate((v - cb2[5].x) / (1 - cb2[5].x))` through a
smoothstep, which rails hard at both ends -- and the buffer measures 40.8%
at exactly 0 and 49.3% at exactly 255. The plausible second draw is
`ps_8A08FF781272C5F6`, two instructions that splat one interpolated
scalar -- a fill.

Two things confirm it independently of reading any shader, and either alone
would be enough:

- **It cannot be occlusion, from the pixels.** Occlusion is a local
  function of the depth neighbourhood, so a patch of surface that is
  locally flat with nothing near it must come out unoccluded whatever the
  strength, the bias or the exponent. Take every 7x7 window of the dump
  (28x28 render pixels) that lies wholly on geometry and whose depth varies
  by less than 2% across the window: eye0 has 78,289 of them and **75.3%
  read exactly 0**; restrict to beyond 50 units and 44,437 windows read
  **97.3% exactly 0, mean 3.4 of 255**. Eye1 gives 80.1% and 96.6%. Forty
  thousand flat, isolated patches reading fully occluded is not an
  occlusion buffer. It is a surface facing away from the sun.
- **Its consumer says so, and this tree said so first.** The deferred
  lighting resolve `ps_7CECABDE34FFBE9E` loads this target at `t3`, picks a
  channel with `dp3 r0.x, r0.xyzx, cb2[44].xyzx`, and then multiplies **both
  the diffuse and the specular** by that one scalar
  (`mul r0.yzw, r0.xxxx, r1.xxyz` and `mul r1.xyz, r0.xxxx, r2.xyzx`).
  Ambient occlusion does not scale direct specular; a light-visibility term
  does. That hash is not a new find either: `7CECABDE34FFBE9E` is the same
  deferred resolve the black-planet hunt named on 2026-08-30
  (`resolve_probe.h:1-23`; the write-up is
  [scanner-body.md](scanner-body.md), which that header still calls
  `docs/black-body.md`), and
  `resolve_probe.h:44` already calls its `t3` blue channel "the shadow
  mask". This tree had the buffer identified before this document guessed
  at it, and the guess would not have been made if the two hunts' notes had
  been read together.

So the eye diff measures the shadow mask. For the record, it says the
shadow mask is fine: `diff_eye_split.py` reports

```
stage 5  632x676 fmt=60   tiles differing 70.6%  balance -0.00  worst +1.0000  draws 2/2  [1148/1638 tiles]
```

and the 70.6% is the differ's whole-frame registration aligning one
disparity across a cockpit at a metre and a ring at a kilometre. Registered
tile by tile instead -- each 32x32 dump-pixel tile aligned on the *rendered
image* and that same shift applied to the other buffers, which takes
parallax out by construction -- over the 131 tiles that register at
r >= 0.70 and carry detail:

| after per-tile registration | median r | 10th percentile |
|---|---|---|
| rendered image (registered on) | 0.974 | 0.940 |
| linear depth | 0.939 | 0.877 |
| shadow mask | 0.950 | 0.394 |

The mask agrees between the eyes marginally better than the depth does
(paired median +0.004); its level difference is 0.28% of full scale with
the darker eye a coin flip tile to tile (eye0 darker on 48.9%); and the
whole-frame disparity is +616 render pixels, matching the linear depth's
+648 and the HDR image's +616. **Shadows are not the reporter's fault.**

**Q7 therefore stays open**, and Q1 and Q3 only half close: the tier's
`ResolutionScale` of 1.0 says the occlusion is eye-sized, and nothing
photographed it.

Two more gates, for whoever aims the next dump:

- It writes **every fourth texel of every fourth row**
  (`eye_split.cpp:29-38`). Anything finer than four render pixels is gone,
  and a kernel rotation on a four-pixel lattice is precisely that scale --
  so even with the UAV gate lifted, this dump could not see the grain
  mechanism C predicts.
- Its size gate is within two pixels of the published eye
  (`near2`, `vscreen.cpp:876`; `targetIsEyeSized`, `vscreen.cpp:1012`), so
  at `Low` or `Medium`, where `ResolutionScale` is 0.25 or 0.5, nothing of
  the chain would be captured at all.

**Q10** is answerable anyway: eye0 is the eye rendered first
(`eye_split.cpp:127-130`), and on the one buffer that was photographed neither
eye is the worse one.

### Step 5: the shader dump could never have held it

The reporter's shader dump is complete and useful, and it cannot contain
the pass. 348 blobs arrived, 200 pixel shaders and 148 vertex shaders and
no compute shader at all, because the dump on that build writes only `vs`
and `ps`. Every stage of Elite's HBAO ends in `_CS`. (`192a36d`, later the
same day, added the `cs` case at `device_hook.cpp:680` -- from another
workstream, for another reason. It is on `main` and in no release, so it
does not help this capture, and it changes everything about the next one.)

All 200 pixel shaders were disassembled and read anyway, twice, under two
different briefs. What that found:

- `ps_CB95394B50D737D6`, five instructions, `c/(z+d)`, one channel out --
  the depth linearisation, matching the `R32_TYPELESS` target that takes
  one draw. This is the only member of the occlusion chain identified.
- No horizon sweep, no kernel rotation, no small-table lookup indexed by a
  pixel position modulo its size, anywhere in the 200. The near misses were
  each run down and each is something else: SMAA's three vertex shaders and
  its blending-weight pixel shader (`ps_BAB75803059C271D`, the `0.8281`
  search test and the 1/160, 1/560 area-texture pixel size); a depth-aware
  upsample of a half-resolution colour layer (`ps_07B3F82100F29401`); and
  the one genuine separable cross-bilateral blur, `ps_DE3602618E7E7F7E`,
  which cannot be the occlusion's because it filters four channels, gates
  empty pixels to **zero** rather than one -- an occlusion's empty value is
  unoccluded, not black -- and normalises its depth difference against a
  kilometre-scale clamp.

A further check dates the dump against the session: no shader was created
after 12:19:57, and the reporter set ambient occlusion to its top tier at
about 12:20:5x, so the AO shaders existed before that -- they were created
at load, while the setting was still on from the previous session. The
dump is not missing them because it started late. It is missing them
because they are compute.

### Where that leaves it

| | | |
|---|---|---|
| **A** | the seed steps per pass | **open.** `c_jitter` is a constant of the pass and the shape A needs; whether it advances per pass is unmeasured, and `census_cb_watch` on the dispatch is what says. |
| **B** | one eye per frame | **closed**, by the worksheet: a stale eye equals the current one when nothing moves, and the fault is unchanged at rest. The dump adds nothing here, having photographed the wrong buffer. |
| **C** | the noise is anchored to the pixel grid | **named, and its premise read.** The deinterleave is by pixel modulo four and the rotation follows the layer; the alternative path samples a tiling table at the pixel grid. What is not yet measured is that this is what the reporter is seeing. |
| **C2** | the resolution | **closed.** The reported tier is `ResolutionScale` 1.0, and the fault is unchanged at 0.25. |
| **D** | EDVR | **closed**, by the worksheet. |

C is the mechanism the evidence names and A is not excluded, and a single
census settles both -- it names the dispatch, and `census_cb_watch` on that
name reads `c_jitter` per eye. Nothing else is worth building first.

*That last sentence is half wrong, and the second capture is what showed
it: the census does name the dispatch, but `census_cb_watch` cannot reach
`c_jitter`, which turns out to live in a shader-resource buffer rather than
a constant buffer. See
[What the second capture said](#what-the-second-capture-said).*

### The second capture, as it was asked for

One more session, nothing new built into the game, and the census armed
from a clock so that no key has to be pressed and no window has to be
focused. Everything the reporter has to do is put five lines in a file and
sit still twice.

```
[fix]
temporal_aa = off

[log]
max_mb = 32

[advanced]
census_offscreen = 1
census_frames = 2
census_lines = 16384
census_at_ms = 360000,720000
glare_shader_dump = 0
```

Four things about that block:

- `census_at_ms` is read **once, at the first frame**
  (`draw_census.cpp:1292-1296`), so it must be in the file before the game
  launches; editing it mid-session does nothing. Up to eight moments,
  comma separated (`kMaxSchedule`, `draw_census.cpp:80`). Each one arms a
  census that records offscreen draws whatever `census_offscreen` says
  (`draw_census.cpp:1320-1322`), which is what a compute chain needs.
- `log.max_mb = 32` is the line Phase 0 above should have carried and did
  not. The default cap is 4 MB (`log.cpp:105-106`) and a census at 16384
  lines is about 2.3 MB (`draw_census.cpp:40-41`), so two or three of them
  plus an ordinary session's log silently stops the writer; a census that
  loses its end line is dropped by the differ rather than half-trusted
  (`diff_draw_census.py:144-146`). A truncated log looks exactly like a
  capture that worked.
- `glare_shader_dump` can go off. It cost the reporter a restart and 348
  files and could never have held a compute shader.
- The two moments are six and twelve minutes after the game's first drawn
  frame, and the design is the same as Phase 0's: **the first with ambient
  occlusion Off, the second with it at the top tier**, parked in the ring,
  still, looking at a cracked fragment both times, and not in the options
  menu when the clock passes either moment. The AO-off census is the
  baseline, and the `ch=` hashes present only in the second are the chain.

**The differ will not do that comparison, and this is the third instrument
change.** `tools/diff_draw_census.py` parses `DC` *draw* lines only
(`DRAW_RE`, `diff_draw_census.py:62`); a `DCX` dispatch line
(`DCX %u #%u n=%u,%u,%u ch=%016llX u=...`, `draw_census.cpp:1273`) is
skipped, and the file's own note about the `bl=` tail says exactly what
that costs: a line the regex misses is a line the tool reports as absent,
in the same words it would use if the effect had not been captured. Until
it reads `DCX`, the comparison is done by hand -- the `ch=` values in the
AO-on census that do not appear in the AO-off one -- which for five entry
points is a `sort | uniq` and not a hardship, but it must be done knowingly.

With the dispatches named, Phase 2 becomes, in order:

```
[advanced]
census_skip_dispatch = <ch>          # the occlusion vanishes: the right pass
census_cb_watch      = <ch>          # A's gate; also dumps a CS's b0 and b1
census_cb_slot       = 0

[experimental]
dispatch_pair_sync   = <ch>:all      # both eyes read one eye's occlusion
```

`census_skip_dispatch` takes up to four comma-separated hashes and
`dispatch_pair_sync` one sixteen-digit hash with optional `:r` and `:all`
suffixes; `:all` is needed because its default gate is the FSS scanner
being up. `census_cb_watch` matches compute shaders and dumps b0 and b1 as
the DCW lines' x and y slots (`edvr.ini`, the `census_cb_watch` block).

## What the second capture said

It arrived on 2026-09-10, three days later, and the reporter improved on
the instructions in two ways worth recording. They did not use the clock:
they pressed the key and **watched for the log line confirming it had been
taken**, which is the same lesson from the other end. And they took the
censuses in the reverse of the order asked for -- **census 1 with ambient
occlusion at High, census 2 with it Off** -- and said so.

Both censuses are complete: `truncated=0`, `overflow=0`, both `DC end`
lines present, 1.82 MB of log under the default 4 MB cap because two
censuses at these scene sizes cost 2450 and 2435 lines, not the 16384 the
cap allows.

```
[14:25:23.602] DC begin census=1 frames=2 frame=28749 offscreen=yes
[14:25:23.649] DC end census=1 draws=764 off=1317 copies=65 disp=87 lines=2450 interned=801 overflow=0 truncated=0
[14:26:23.246] DC begin census=2 frames=2 frame=33014 offscreen=yes
[14:26:23.292] DC end census=2 draws=748 off=1317 copies=76 disp=76 lines=2435 interned=788 overflow=0 truncated=0
```

The reversed order needs no correction and no trust: **87 dispatches
against 76**, and the hashes that appear in one census and not the other
appear in census 1. The data says which one had the effect.

### The chain, named

The comparison was done by hand, for the reason the section above gives:
`diff_draw_census.py` parses draw lines only. Grouping every `DCX` line by
its `ch=` and subtracting leaves **three compute shaders present with
ambient occlusion on and absent with it off**, four dispatches each across
the census's two frames:

| hash | reads | writes | thread groups |
|---|---|---|---|
| `FB277B33F0865348` | the eye's linear depth, 1896x2028 `R32_FLOAT`, at `s0` and `s1` | 474x507 `R32_FLOAT` at `u0` | 119x127x1 |
| `9347F8FC2DCE0248` | the G-buffer normals 1896x2028 `R10G10B10A2` at `s0`; the 474x507 depth at `s1`; **a 768-byte buffer at `s2`** | 474x507 `R8_UNORM` at `u0` | 30x32x**16** |
| `D31E7812990B19A6` | the 474x507 depth at `s0` and the 474x507 occlusion at `s1` | **1896x2028 `R8_UNORM`** at `u0` | 119x127x1 |

Four numbers make that unambiguous, and they are the whole of mechanism C:

- **1896 / 4 = 474 and 2028 / 4 = 507, exactly.** The middle stage's
  layers are a quarter of the frame in each axis.
- **The middle stage's z is 16.** Sixteen layers, dispatched together. That
  is the 4x4 lattice, and which layer a pixel is estimated in is its
  position modulo four.
- **768 bytes is 16 x 48** -- sixteen entries of three float4s, indexed by
  the layer. That is `g_performAOData`, and it is where `c_float2Offset`
  and `c_jitter` live. It is bound at `s2` rather than as a constant buffer
  because all sixteen passes run in one dispatch and the shader indexes it
  by `SV_DispatchThreadID.z`.
- The outer two stages dispatch 119x127 groups over 1896x2028 (118.5 and
  126.75, rounded up) and the middle 30x32 over 474x507 (29.6 and 31.7).
  16x16 threads throughout.

Against the executable's five entry points, the mapping is forced:
`FB277B33F0865348` is `HBAO_CONSTRUCT_LINEAR_DEPTH_ONLY_CS` rather than the
normal-and-depth variant, because it writes one target and no normal
texture and the AO pass takes its normals from the G-buffer instead;
`9347F8FC2DCE0248` is `HBAO_PERFORM_AO_CS`; and `D31E7812990B19A6` is
`HBAO_REINTERLEAVE_AND_BLUR_CS` and not the `_UPSAMPLE_` variant, which is
what `ResolutionScale` 1.0 predicts. The two the game did not run are the
two a lower tier would.

### Q1 and Q2, measured at last

In frame 0 the three dispatches run at `q=956,957,958` on one eye's depth
and again at `q=974,975,976` on the other's, with every resource distinct
between them -- separate depth inputs, separate layer arrays, separate
outputs, separate constant buffers. Frame 1 repeats it. `q=` is one shared
per-frame ordinal across draws, copies, clears and dispatches alike
(`draw_census.h`), so adjacency in `q` is adjacency in the frame.

**Both eyes, every frame, nothing alternating. B is closed by measurement
now, not only by the worksheet.** And Q2's answer, taken from the
executable's symbol names three days ago, is confirmed from the census
itself: dispatches.

### The consumer, and Q3

The differ, run with the AO-off census as the baseline (`--a 2 --b 1`,
because the reporter reversed the order), names one draw and only one:

```
DrawInstanced n=3  target=tex1896x2028f27  depth=none  samples=tex1896x2028f60
  topology=tristrip  stride=20  vshader=DEF19B035D5EDEDC
  draws per frame: 2,2   render targets hit: @137 x2, @329 x2
  census_skip spec: N:3
```

A three-vertex full-screen draw, twice a frame into two different targets,
sampling the 1896x2028 one-channel target the chain's last dispatch wrote.
That is `RenderSSAO`, which the executable lists beside
`RenderDirectionalLights` and the rest of the deferred lighting techniques.
`REMOVED` is empty and `CHANGED` is scene churn -- instance counts drifting
by a percent or two between two captures a minute apart, which is what a
live ring does.

**Q3 answered:** the occlusion target is `R8_TYPELESS` viewed as
`R8_UNORM`, at the scene's render size.

One footnote that closes Q4 twice over. The render size this session is
1896x2028, not the 2528x2704 the runtime publishes as an eye, because the
reporter's `HMDRenderTargetMultiplier` is 0.750 in this bundle where it was
1.000 in the first. So the occlusion buffer misses the eye-split dump's
two-pixel size gate by six hundred pixels quite apart from being a UAV that
no draw ever binds. Two independent reasons that instrument could never
have photographed it.

### Where A and C stand

**C is measured.** Not inferred from symbol names any more: the layer a
pixel is estimated in is its screen position modulo four, the rotation is
one of sixteen entries selected by that layer, and the same surface point
falls on a different pixel -- and therefore in a different layer, under a
different rotation -- in each eye. There is nothing left to capture for it.

**A is not settled, and no shipped instrument can settle it.** The per-pass
table is bound at `s2`, a shader-resource slot, and `census_cb_watch` reads
a dispatch's constant buffers only -- `CSGetConstantBuffers(0, 2, ...)` in
`cbWatchOnDispatch` (`draw_census.cpp`) -- so the 192-byte `b0` of globals
and a `b1` are the whole of its reach. Whether the two eyes' 768-byte
tables hold the same sixteen rotations is unmeasured, and the census cannot
say: it records what is bound, not what is written.

It may not matter. C is present by construction whatever those tables hold.
If they are identical the fault is C alone; if they differ, that is A **on
top of** C, and both live inside the same three dispatches. One shipped key
tests that in one flight -- and a build carrying `192a36d` would put
`9347F8FC2DCE0248`'s own bytecode on disk, which answers A by reading the
shader rather than by guessing at its inputs. See the instrument list
below; that commit exists and is unreleased.

### Phase 2, as it can be typed today

Both of these are shipped keys and neither needs anything built. One flight
each, in this order.

```
[advanced]
census_skip_dispatch = 9347F8FC2DCE0248
```

The occlusion should vanish, exactly as turning the setting off does. That
proves the census named the right pass, from EDVR's side, which the setting
cannot prove. Up to four comma-separated hashes if the whole chain is
wanted; the scene may look very wrong while it is set, and that is the
probe working.

```
[experimental]
dispatch_pair_sync = D31E7812990B19A6:all
```

`D31E7812990B19A6` is the reinterleave, and its `u0` is the finished
full-resolution occlusion -- so this copies the first eye's occlusion over
the second's after it runs. It is **not a fix**: one eye's occlusion is
wrong for the other at every near silhouette, and that wrongness is the
point of the measurement. What survives is the parallax the eyes are
*supposed* to disagree about; what disappears is everything the pass itself
introduced. If the reporter's cracks stop disagreeing under it, the fault
is entirely inside these three dispatches and the fix is a substitution.
The `:all` suffix is required: the key's default gate is the FSS scanner
being up, and this pass runs game-wide.

### What this capture changes about the instruments

The list from three days ago reorders, and gains two entries.

1. **Dump compute shaders -- already built, not yet released.** This was
   written up as the blocker and it is not one: `192a36d` added it on
   2026-09-07, the same day this document said it was missing, from the
   per-object-motion work rather than from here
   (`dumpShaderBlob(L"cs", ...)`, `device_hook.cpp:680`). The hash it names
   the file by is `fnv1a64` of the bytecode, which is the same value
   `lookupShaderHash` gives the census for its `ch=` column
   (`draw_census.cpp:1345`), so a dump on a build carrying that commit
   writes **`cs_9347F8FC2DCE0248.dxbc`** and the pass's own code can be
   read at last. It is on `main` and in no release: the reporter is on
   v0.14.0 and the newest tag, v0.14.1, predates the commit by hours.
   **Everything C1 needs, and the only desk-side answer to A, is behind
   shipping a build with it.**
2. **Read a dispatch's SRV-bound buffer** (new, and the only genuinely
   missing one). `cbWatchOnDispatch` would need a companion that maps a
   named dispatch's `s` slot rather than its `b` slots. It is the direct
   way to compare the two eyes' sixteen rotations -- though if item 1
   ships, the disassembly may make it unnecessary by showing what the
   shader does with the table rather than what is in it.
3. **A census key that does not need window focus.** Still wanted. The
   reporter worked around it by watching for the confirmation line, which
   works and should not have to be discovered.
4. **A differ that reads `DCX`.** Still wanted. The comparison above took
   twenty lines of Python that should live in `diff_draw_census.py`, and
   the risk it carries is the one that file already documents: a line the
   regex misses reads as a line that was not there.
5. **A staging copy of a named dispatch's `u0`.** Drops to last. It was
   the way to photograph the occlusion buffer, and the census has now
   answered everything that photograph was for.

## Phase 1: reading the capture

At the desk, in this order; each step names what it answers.

1. **Name the pass.** `python tools/diff_draw_census.py <gfx-log>`
   compares the last two censuses, the earlier as the baseline, which is
   why Phase 0 takes the AO-off census first (with two sessions, pass
   both logs, baseline first). The `ADDED` section is
   the pass and its blur: for each eye, one full-screen draw (`D` or `I`,
   three to six vertices) whose `s0` is the eye's depth and whose `r=`
   resolves in the interned table to a one-channel target, or a `DCX`
   line whose `u0` is that target and whose `s0` is the depth; then one
   or two draws that read the target and write another of the same shape;
   then the lighting resolve carrying the result in one of its slots.
   Their hashes (`vh=`, `ph=`, `ch=`) are the pass's names for every
   probe after this.
2. **Count it per eye.** Pair the pass's lines within each frame by `q=`
   against the two eyes' depth clears (DCL `D`). Two per frame, one per
   eye, every frame, rules out B. One per frame, alternating, is B.
3. **Size the target.** The interned table at the census's end gives the
   target's width, height and format (`vf=`). Eye-sized is the simple
   case; half-sized makes C2 real and changes what the dump can see (Q4).
4. **Diff the eyes.** `python tools/diff_eye_split.py edvr_logs/dumps
   edvr_logs/edvr_gfx_*.log`. Find the one-channel stage in the manifest
   and read its `balance` and tile columns: a one-sided imbalance is a
   level difference (A or B); a zero-balance, uncorrelated grain is C.
5. **Read the shader.** Disassemble the pass's pixel or compute shader
   from `edvr_logs\shaders`, the way the sun-glare and grain hunts read
   theirs. What to look for: a sample from a small square texture indexed
   by the pixel's position modulo its size (C's table, and its slot); a
   constant added to that lookup or to the rotation (A's seed, and its
   offset in the buffer); the sample count and radius (what the levels
   change); and whether the position it reconstructs is view-space from
   depth (what C1 has to extend to world space).
6. **Decide.** The table under "The short version" is the decision table.
   A's gate is step 5's constant plus a `census_cb_watch` flight; B's is
   step 2; C's is steps 4 and 5 plus the uniform-slot probe.

## Phase 2: live probes

One flight each, on the reporter's rig, each an existing key unless
marked. Order by what each rules out.

- **Skip the pass.** `census_skip = vs:<vh>` for a draw, or
  `census_skip_dispatch = <ch>` for a dispatch. The occlusion vanishes,
  which proves the census named the right pass -- and if the eyes then
  agree at the cracks, the whole fault is inside it. The same result as
  turning the setting off, arrived at from EDVR's side, which is the
  proof the setting cannot give.
- **Watch its constants.** `census_cb_watch = <hash>`, with
  `census_cb_slot` set to whichever slot step 5 named, and one more
  census. A's signature or its absence.
- **Equalise its output.** For a compute pass,
  `experimental.dispatch_pair_sync = <ch>:all` copies the first eye's
  product over the second's after it runs. Not a fix -- one eye's
  occlusion is wrong for the other at every near silhouette -- but a
  measurement: what remains when both eyes read one occlusion is the
  parallax, and what disappears is the pattern. A draw-shaped sibling
  (copy the first eye's target over the second's after its draw) would be
  small and is the second instrument this document would build if the
  pass is a draw.
- **Flatten its noise** (new, small). The uniform-slot probe from C,
  `hud_grain`'s substitution keyed by shader hash and slot. Decisive for C
  in one flight.

## The fix and its key

Whichever mechanism wins, the fix ships as one key in the
"when the eyes disagree" family, one mechanism underneath, the way
`fss_eye_sync` does:

```
# Make ambient occlusion agree between the eyes. ...
# ui: Ambient occlusion agrees between the eyes | choices on, off
ao_eye_sync = off
```

The name is a placeholder until the mechanism is known; the settings
schema needs the `# ui:` line and `tools/check_config_contract.py` needs
the code to read exactly what the ini defines. Off until the reporter has
flown it; on by default only if it never engages on a rig without the
fault (A and B) -- C1 engages everywhere the setting is on and stays a
choice, because it changes the look of a setting the player chose.

Under every mechanism the substitution discipline holds: the game's
buffers are never written, every substitution is restored after the draw
or dispatch it wrapped, every stand-down is a log line naming why, and a
matcher that stops matching after a game update leaves the game drawing
stock. The exposure fix's lesson applies to naming the pass: dispatch
shape is not evidence, and neither is a draw's vertex count; what
identifies the pass is what it reads, what it writes, and its
disassembly ([eye-brightness.md](eye-brightness.md), "a note on method").

## What a fix inside the game would look like

For whoever might make one there. Share the kernel's per-frame rotation
seed across the stereo pair rather than advancing it per pass; take the
per-pixel rotation from a hash of world position, or from a screen
position registered per eye to the same surface, rather than from the
pixel grid; and run the pass for both eyes every frame at one quality.
None of it costs a pass. What should not be removed is the difference
that is real: the two eyes see different occlusion at every silhouette
because they see different silhouettes, and a fix that makes the eyes
identical has made one of them wrong.

## Open questions

Each with what answers it, and what the first capture did to it.

1. **Does the pass run per eye?** Still open on its own evidence. The
   worksheet closed B independently (unchanged at rest), and the dump
   photographed the shadow mask rather than the occlusion, so it says
   nothing about this. Phase 1 step 2 on the next census, counting DCX
   lines against the two eyes' depth clears.
2. **Draw or dispatch?** **ANSWERED: dispatch.** Read from the game
   executable of build 332841 -- `HBAO_CONSTRUCT_LINEAR_DEPTH_ONLY_CS`,
   `HBAO_CONSTRUCT_NORMAL_AND_DEPTH_CS`, `HBAO_PERFORM_AO_CS`,
   `HBAO_REINTERLEAVE_AND_BLUR_CS`, `HBAO_REINTERLEAVE_BLUR_UPSAMPLE_CS`.
   So the probes are `census_skip_dispatch`, `dispatch_pair_sync` and
   `census_cb_watch` on a compute hash, and the fix is a dispatch-shaped
   substitution. A draw-shaped tail exists too (`ReinterleaveAOPS`).
3. **What size and format is the target?** **ANSWERED by the second
   census.** The final occlusion is 1896x2028 `R8_TYPELESS` viewed as
   `R8_UNORM` -- the scene's render size, which on that session is 0.750 of
   the published eye. The chain's middle is a 474x507 `R8_UNORM` array of
   sixteen layers, fed by a 474x507 `R32_FLOAT` depth array. The dump's
   eye-sized `R8_TYPELESS` target was the shadow mask, not this.
4. **Did the dump catch it?** **ANSWERED: no, and it cannot as built.**
   `eye_split` records the render target bound at slot 0 of a *draw*
   (`eye_split.cpp:244-250`, from `vscreen.cpp:2053`), and the HBAO chain
   writes UAVs (`g_performAOOutputTexture`,
   `g_reinterleaveAndBlurOutput`), which it cannot see; nor can it see a
   dispatch. Two further gates would bite even if that one were lifted: it
   writes every fourth texel of every fourth row (`eye_split.cpp:29-38`),
   which aliases away a rotation grain on a four-pixel lattice, and its
   size gate is within two pixels of the eye (`near2`,
   `vscreen.cpp:876`), so at Low or Medium nothing of the chain is
   captured at all -- and the second capture adds a third gate for free:
   the occlusion target is the render size, 1896x2028, which misses the
   two-pixel eye-size test by six hundred pixels on its own. Copying a
   named dispatch's `u0` at the boundary would lift the first of those,
   but the census has since answered everything that photograph was for,
   so it is now the LAST instrument change this hunt wants rather than the
   first.
5. **Does a constant step per pass?** **The one question still open, and
   no shipped instrument reaches it.** The second census located the
   table: `c_jitter` and `c_float2Offset` live in a 768-byte buffer -- 16
   entries of 48 bytes, one per deinterleaved layer -- bound at `s2` of
   `9347F8FC2DCE0248`, a shader-resource slot, because all sixteen layers
   run in one dispatch and the shader indexes it by the thread id's z.
   `census_cb_watch` reads a dispatch's CONSTANT buffers only
   (`CSGetConstantBuffers(0, 2, ...)` in `cbWatchOnDispatch`), so it can
   dump the 192-byte `b0` of globals and nothing else. Whether the two
   eyes' tables hold the same sixteen rotations needs an instrument that
   does not exist. Note that C stands either way: if they are identical
   the fault is C alone, and if they differ that is A on top of C.
6. **Is there a noise table, and at which slot?** **ANSWERED: `s2` of
   `9347F8FC2DCE0248`, 768 bytes, sixteen entries.** The deinterleaved
   path is the one this tier runs -- the middle dispatch's z is 16 and its
   layers are exactly a quarter of the frame in each axis -- so the
   rotation is selected by the pixel's position modulo four. The
   `c_useJitterTexture` / `c_randomTexture` path is present in the code and
   not bound here.
7. **Level or pattern?** **Still open.** The reporter says level; nothing
   in the capture photographed the occlusion, so the dump cannot second
   them. What it did measure is the sun-shadow mask, which agrees between
   the eyes as well as the linear depth does (0.28% of full scale, the
   darker eye a coin flip tile to tile) -- so shadows are not the fault
   and can be set aside.
8. **Stock, temporal AA off, and Low?** ANSWERED by the worksheet: all
   three still show it. Two threads left -- Low is a quarter-resolution
   pass with a blur sharpness of 8 against 256, and "looks exactly the
   same as High" is worth re-asking; and the second capture was taken at
   `HMDRenderTargetMultiplier` 0.750 where the first was at 1.000, so the
   occlusion has now run at two different resolutions on this rig without
   the report changing.
9. **What do the levels change?** **ANSWERED**, read from the reporter's
   own `GraphicsConfiguration.xml`: only `HBAO2_Bias`,
   `HBAO2_BlurSharpness` and `ResolutionScale`. Not the sample count, not
   the radius, not the strength.
10. **Which eye renders first on this rig, and is it the worse one?**
    Eye0 renders first (`eye_split.cpp:127-130`), and the reporter says
    neither eye is worse. On the one buffer that was photographed the
    shadow mask -- neither is, measured. The scanner-body hunt's pattern,
    the healthy eye drawing first, has no unhealthy eye here to name.
11. **Everywhere, or only asteroids?** Open. The worksheet says "only icy
    rings so far" and the arm and the hangar are still unlooked at
    ([Beyond asteroids](#beyond-asteroids)).
12. **Is the deinterleave on at this tier, and how many layers?**
    **ANSWERED: on, sixteen layers.** `9347F8FC2DCE0248` dispatches
    30x32x**16** groups over layers of 474x507, and 1896/4 = 474 and
    2028/4 = 507 exactly. The `SeparateDepthTextureInto8Targets` name
    belongs to the pixel-shader path, which this build does not run.
13. **Do the two eyes' sixteen rotations differ?** New, and it is Q5 from
    the other end -- the only thing between this hunt and a fix.

## What this document does not do

It does not pick the mechanism, and it does not promise C2. It names the
one capture that picks the mechanism, the probes that confirm it, and the
fix that each one implies, so that the session after the capture builds
rather than guesses. Every guess this repo's hunts made about which draw
was the body was wrong until a live probe settled it
([eye-split.md](eye-split.md)); the same is assumed here of every
sentence above marked believed.
