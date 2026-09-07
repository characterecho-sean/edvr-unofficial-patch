# Ambient occlusion that disagrees between the eyes: a design for the hunt

*A design document, written before any capture. Written 2026-09-07 on
branch `claude/asteroid-ao-inconsistency-xt19n7` off main `dc3ebad`. Claims
about EDVR cite the source; claims about the game are labelled measured
(this repo's censuses, dumps and disassemblies), read (taken from a
captured shader's bytecode), or believed; what only a live session can
settle is collected under Phase 0. Nothing here is built, and nothing here
has been captured: the ambient-occlusion pass has never appeared on a
census by name, and no file in this tree mentions it.*

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
search, 2026-09-07), and this repo's own tracker has none.

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
- **The eye-split dump has probably already photographed the buffer.** The
  measured field frame carried sixteen eye-sized targets, among them a
  2324x2392 target at format 60, `R8_TYPELESS`, one byte a texel
  (`src/d3d11/eye_split.cpp:65`; [eye-split.md](eye-split.md)). A
  one-channel eye-sized target is the shape of an occlusion buffer, and
  `tools/diff_eye_split.py` decodes that format
  (`diff_eye_split.py:157`). That it *is* the occlusion buffer is
  believed; its `EYESPLIT` manifest line (`eye_split.cpp:187`) carries the
  draw count into it, and a full-screen pass is one draw where the
  geometry buffer is hundreds.
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

Each with what answers it. None is answerable from the desk.

1. **Does the pass run per eye?** Two families of lines per frame, or one
   read by both resolves (Phase 1, steps 1-2).
2. **Draw or dispatch?** `DC`/`DCO` or `DCX` (step 1). It decides which
   substitution, which skip key, and whether `dispatch_pair_sync` applies.
3. **What size and format is the target?** The interned table (step 3).
4. **Did the dump catch it?** `eye_split` records the render target bound
   at each *eye-texture* draw, and an eye-texture draw is one into a
   target of the eye's own size, within two pixels (`vScreenIsEyeSized`,
   `vscreen.h:95`; `near2` in `vscreen.cpp:876`).
   A half-sized target is inside the census's shape gate
   (`eyeShapedAtScale`, `vscreen.h:68`, 40% to 250%) but outside the
   dump's, and a target written only by a dispatch is outside it
   entirely. Widening the dump's gate to the shape test is a two-line
   change; copying a named dispatch's `u0` at the boundary is
   `dispatch_pair_sync`'s copy pointed at a staging texture. Either is
   the first instrument change this hunt makes, if Q3 says so.
5. **Does a constant step per pass?** `census_cb_watch` (Phase 2).
6. **Is there a noise table, and at which slot?** The pass's `s=` column
   and the interned table's size for it; the disassembly's modulo (step 5).
7. **Level or pattern?** The eye-split diff's `balance` and the reporter's
   own account (step 4, Phase 0).
8. **Stock, temporal AA off, and Low?** The reporter (Phase 0).
9. **What do the levels change?** The `GraphicsConfiguration.xml` in the
   Save logs bundle, read rather than believed.
10. **Which eye renders first on this rig, and is it the worse one?** The
    dump's `eye0` is the eye rendered first; the reporter names the worse
    eye. The scanner-body hunt found the healthy eye drew first in every
    failing frame, and whether that holds here is one line of the log.

## What this document does not do

It does not pick the mechanism, and it does not promise C2. It names the
one capture that picks the mechanism, the probes that confirm it, and the
fix that each one implies, so that the session after the capture builds
rather than guesses. Every guess this repo's hunts made about which draw
was the body was wrong until a live probe settled it
([eye-split.md](eye-split.md)); the same is assumed here of every
sentence above marked believed.
