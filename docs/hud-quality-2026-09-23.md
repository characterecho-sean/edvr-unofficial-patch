# fix.hud_quality: the cockpit HUD at a higher target quality

## Status

BUILT, NOT FLOWN (the ratio-match rewrite; the classifier-match build was
flown and is superseded, see "Flight 1" below). `hud_quality_test.exe
--self-test` covers the arithmetic and the ratio state machine; no headset
session against this build yet.

State: `fix.hud_quality = off | 1.0 | 1.25`, on the Performance page,
generalises `advanced.surface_inflate` (fss_res.cpp) from a named `WxH` and
an integer 2..4 to a size matched by its RATIO to the game's internal
render resolution (confirmed across two sessions at different resolutions)
and a float factor derived from the game's live HMD Quality. The classifier
(`ui_depth.cpp`) is no longer the matcher -- see "Why the rewrite" -- and is
kept only as a same-session cross-check that labels a match vector/text/icon
for the log.

Open: **gate G9** (`docs/crisp-ui-handoff.md:1017`) -- does inflating a
surface sharpen its TEXT, or only its vector lines. Never flown for any
inflation mechanism. This ships to run that flight, and now also to name
the allocating game function (the new RVA log line) in case the answer is
"build it into the engine instead", the way `vscreen_res.h` already does
for the on-foot screen.

Ruled out: **the classifier as the primary matcher** (2026-09-23, Flight 1).
It learns a surface's size from draws INTO it, which happen after
`CreateTexture2D` -- too late to size the create itself. A panel created
before any draw into it (every session's first one, and every short
menu-only session that never revisits a panel) matches nothing through it.
Kept only as a cross-check counter now.

Next flight: HMD Quality 0.7 or lower, in the **cockpit** (not the main
menu -- see Flight 1), `fix.hud_quality = 1.0`. See "What the first flight
must show".

## Flight 1 (2026-09-23, build 7a47fd3c, gfx log edvr_gfx_20260923_073156.log
and edvr_gfx_20260923_073409.log)

Sean toggled `off -> 1.0 -> 1.25` from the F8 menu several times, mostly
from the **main menu**. `073156.log`: the configure-time line appears at
every toggle; no `fss res: a WxH texture was created` line and no resize
line at all that session -- nothing was tracked. `073409.log` (a later,
longer session): one match, `vector 1346x757 -> 2071x1165` then
`-> 2588x1456` as the target was re-toggled, via the old classifier path.

Two things follow, both addressed in this build:

- **The classifier-timing flaw** above -- fixed by the ratio match.
- **Why nothing renders yet at the main menu, under either mechanism.**
  `vScreenInternalResolution()` (this build's new dependency, `vscreen.h`)
  answers false until the game has rendered over 100 eye-shaped draws in
  one frame (`kSceneEyeDraws`, `vscreen.h`) -- ordinary gameplay reaches
  this in the first loaded frame, but the main menu's own 3D backdrop
  apparently does not, or not reliably: across the ten `edvr_gfx_2026
  0923_*.log` (04:23-07:36) flown today, the promotion log line ("vScreen:
  the world on this rig is rendered at...") never once appears, while it
  appears in six logs from 2026-08-29 (real play sessions). **A ratio
  cannot be matched, named or otherwise, before the internal resolution is
  known** -- this is a lower bar than "the specific panel has been drawn
  into" (which the classifier needed and the ratio match does not), but it
  is still a bar, and testing entirely from the main menu does not clear
  it. `fix.hud_quality` now says so once it is on: "the game's internal
  render resolution is not known yet".

## Why the rewrite: the classifier could never inform the create it needed to

`ui_depth.cpp`'s classifier (`uiDepthNoteOffscreenDraw`) learns a render
target's size from a draw that lands in it, and a draw can only land in a
texture that already exists. `fssResMaybeInflate` runs INSIDE
`CreateTexture2D`, before the texture exists and before any draw of it is
possible. So the classifier could only ever help a texture's SECOND
creation in a session where its first creation was ALSO watched by the
classifier being on (`fix.temporal_aa`) -- Flight 1 shows this is rare in
practice, not the edge case it reads as on paper.

fss_res.h's own two-session census is the fix: interface surfaces are a
fixed fraction of the game's internal render resolution, not of anything
learned from a draw --

    908x1361 at scene 4340x4284       -> 0.2092 x 0.3177
    1363x2042 at scene 6510x6426      -> 0.2094 x 0.3178

agreeing to within a few parts in 10000 across two sessions at two
different resolutions. That fraction is exactly what `CreateTexture2D`
already carries in its own desc, the moment the game asks -- no draw, no
classifier, no prior session even required for the FIRST sighting (only
for using it).

## The mechanism

1. **The ratio table.** `hud_quality_math.h`'s `HudQualityRatioSlot` /
   `hudQualityRatioObserve` is a small, pure (no file I/O, no D3D11) state
   machine: given a candidate's ratio (rounded to four significant
   figures, `hudQualityRatioX10000`) and the internal width it was seen
   at, it returns `kNewCandidate` (not seen before -- recorded, not used),
   `kSameSession` (matches an unconfirmed entry, but at an internal width
   already on file -- no new evidence), or `kConfirmed` (matches at a
   DIFFERENT internal width than it was last seen at -- usable from this
   call on). `fss_res.cpp` owns the table (16 slots) and persists it to
   `<log dir>\hud_quality_ratios.txt` (the same raw-WinAPI-I/O discipline
   as `vscreen_auto_state.cpp`'s `vscreen_auto_eye_width.txt`), loaded
   lazily on the first candidate create. **A ratio is trusted only once
   two DIFFERENT sessions' resolutions have agreed on it** -- a fresh
   install's first-ever sighting of a shape is recorded and logged "seen,
   not resized", never guessed into an inflate.
2. **Which surfaces are candidates.** At each `CreateTexture2D`, a
   single-mip, non-MSAA, render-target-or-depth desc (shared with the
   other two matchers) that is also smaller than the internal resolution
   and non-power-of-two on both axes (fss_res.h: "an odd, non-power-of-two
   render target") is offered to the ratio table. `vScreenInternalResolution()`
   (new, `vscreen.h`/`.cpp`) publishes `vscreen.cpp`'s own `renderW`/`renderH`
   -- the MEASURED (not guessed) internal per-eye render resolution
   already used to decide `vScreenIsEyeSized` -- false before it has been
   measured (Flight 1, above).
3. **The classifier, now a cross-check only.** Once a candidate is
   confirmed and about to be inflated, `uiDepthLearnedSurfaceSizes()`
   (`ui_depth.h`) is asked whether it already knows a surface of this
   exact (pre-inflate) size this session; if so, its family ('V'/'T'/'I')
   labels the log line. If not (the common case at the moment of a FIRST
   confirmation, or whenever `fix.temporal_aa` is off), the match still
   happens -- it is logged as "other", not blocked.
4. **The factor.** Unchanged from the first build: `deviceHookHmdQuality()`
   reads Elite's own `HMDRenderTargetMultiplier` from its newest `.fxcfg`
   (cached one second per thread); `factor = target / that`, applied only
   above a 1% floor, capped at 4. Read fresh at each candidate create, not
   cached at ini-reload.
5. **Why the factor was integer-only, and what fractional needs.**
   Unchanged: `Spec::scale`/`Tracked::scale` are `float` now so the SAME
   viewport/texture-size code serves both matchers; `advanced.surface_inflate`
   keeps its own integer-only syntax on purpose. A texture's Width/Height
   round with `hudQualityRoundDim` (`v*factor + 0.5f`); viewport
   coordinates are native float and need no rounding.
6. **Viewports and scissors.** Unchanged: the `RSSetViewports` hook and its
   draw-time backstop multiply by the tracked float scale; scissor rects,
   which have no set-time hook at all, are corrected only at the draw-time
   backstop, and only when the current rect still spans the pre-inflation
   target exactly (a genuinely narrower, clipping rect is left alone).
7. **Copies and resolves.** Unchanged: detected and logged, not rescaled
   -- no such copy has ever been observed landing in one of these
   surfaces, and box math for an unseen shape would be an untested-hypothesis
   fix.
8. **The RVA instrument (new).** Sean asked whether the size could be
   patched in the engine instead of intercepted after the fact, the way
   `vscreen_res.h` already rewrites the numbers the game forces for the
   on-foot screen. The moment a surface is confirmed and about to be
   inflated, `captureGameCallStack()` (`common/game_call_probe.h`, already
   used by the exit probe and the object-classification probe -- reused
   here, not rebuilt) captures the return-address chain and filters it to
   frames inside the game's own module; the first four are logged once per
   distinct surface SIZE per session, gated on the key being on. This
   names the allocating call for a human to decompile toward a
   `vscreen_res.h`-style engine patch, which would size the game's own
   viewports/scissors/copies itself rather than needing any of the above.

## Log lines

At the first surface resized, and every 30s after while something has
matched:

    hud quality: 1.0 (HMD Quality 0.65 -> factor 1.5385): cockpit panels
    resized 1 (vector 1346x757 -> 2071x1165, text none yet, icon none yet),
    FSS 0, other 0, seen not resized 2, viewports rescaled 6, scissors
    rescaled 0, copies touching one: 0.

"FSS" counts the existing half-eye matcher's own hits while the key is on
(a different code path; fix.hud_quality never causes them, but a flight
with the scanner open alongside the key wants one total picture). "other"
is a ratio match the classifier has not labelled vector/text/icon --
common, since the classifier depends on `fix.temporal_aa` and the ratio
match does not. "seen not resized" is the count of distinct candidate
ratios on file that are not yet confirmed at a second, different internal
resolution.

If the key is on and nothing has matched after a minute:

    hud quality: on but nothing has matched by ratio yet (2 candidate
    ratio(s) seen, none confirmed at a second, different internal
    resolution -- an earlier session's data counts, so this is common only
    on a fresh install or one that has always run the same HMD Quality):
    nothing changed.

A candidate seen but not yet confirmable, capped at 8 lines:

    hud quality: seen, not resized -- a 1346x757 candidate's ratio to the
    internal render resolution is on file but not yet confirmed at a
    second, different resolution. Said at most 8 times.

The RVA instrument, once per distinct size per session:

    hud quality: interface surface 1346x757 (vector) created from game
    RVAs 0x2A1F3C0/0x2A1E8B0/0x2A1D440/0x2A15E20 (4 of 12 captured frames
    were in the game module).

Distinguishing "the new code never ran" from "it ran and did nothing": the
configure-time line fires the moment the key is read, whether or not
anything ever matches. "internal render resolution is not known yet" (once)
means the game never rendered a real scene this session -- test from the
cockpit, not the main menu (Flight 1). "seen, not resized" means a
candidate exists but is unconfirmed. The resize/summary line is the only
one that means the feature acted.

## What the first flight must show

HMD Quality 0.7 or lower (Elite's own graphics options), `fix.hud_quality
= 1.0`, **flown from inside the cockpit or another real rendered scene**,
not only from the main menu (Flight 1's session never measured an internal
resolution at all):

1. The resize line, naming at least one surface. "seen not resized" > 0
   with nothing resized on a FRESH install's first session is expected --
   fss_res.h's own two-resolution proof needs a second session at a
   different HMD Quality to confirm anything; that second session should
   show a resize instead.
2. The HUD text judged in the headset, at `off`, `1.0` and `1.25` in turn
   (a menu trip between each, since surfaces only resize at their next
   creation) -- sharper or not. This is gate G9 itself: inflation has been
   measured reaching vector lines (the widget shader draws in a normalised
   space, so more pixels is more detail by construction); the glyph text
   samples a 2048x2048 A8 atlas whose own lifetime under inflation is
   unmeasured, and no inflation flight has ever been judged on the letters
   before this one.
3. Frame time unchanged (the target-indicator arc's own inflation flights
   measured no cost).
4. The RVA line for whatever resized, for the record -- it does not need
   judging in the headset, only capturing.
5. **If the text is not sharper**, gate G9 fails and the lever beyond
   inflation is Design A in `docs/crisp-ui-handoff.md` (`## A5`) -- the
   extra texels this mechanism creates do not survive the render-resolution
   composite before EDVR's own upscale. Design A composites at OUTPUT
   resolution, but the panel TEXTURES it samples are still rendered at
   scene resolution -- so the cockpit text most likely needs BOTH this
   inflation and Design A's layer, not either alone; the two keys are
   meant to unify once both have flown (`fix.hud_quality` driving the
   inflation target when set). This build does not attempt Design A.

**Do not judge on the target direction indicator.** The target-indicator-
hunt arc already ruled that artefact's sprite is laid out at a fixed pixel
size inside its surface and does not sharpen however big the surface is.

## Contract

`fix.hud_quality` documented in `edvr.ini` under `[fix]` with a `# ui:`
line (Performance page, matching `fix.settlement_detail`'s precedent);
read in `fss_res.cpp`; asserted in `tools/config_test/config_test.cpp`.
Contract count +1 over pre-hud_quality (one key, read and documented; 261
read / 261 documented with this build). `hud_quality_test.exe --self-test`
covers the parsing, the factor arithmetic, the rounding, and the ratio
state machine (new-candidate / same-session / confirmed / table-full, and
the founding two-session census numbers themselves); auto-discovered by
`tools/run_jobs.py`'s `:rig_<label>` scan in `build.bat`'s gate.

## Doubt, stated plainly

- The ratio table's tolerance (10 ten-thousandths, 0.1%) is chosen from
  ONE documented two-session comparison (fss_res.h's own census); it is
  not independently re-derived from today's flight, since today's flight
  never logged an internal resolution to pair with a panel size.
- `vScreenInternalResolution()` uses the STRONG promotion
  (`renderW`/`renderH`, eye-shape-corroborated) rather than the weaker
  "busiest render target" one (`sceneW`/`sceneH` alone) that fires more
  often but is not guaranteed to be one undistorted eye's worth of scene
  -- chosen for correctness over availability; the cost is that a session
  which never triggers the strong promotion (Flight 1) matches nothing,
  which the log now says plainly rather than guessing with a shakier
  number.
- The inventory of every offscreen UI surface (shader family, ratio,
  what it shows) is in `docs/crisp-ui-handoff.md`'s "Offscreen UI surface
  inventory" section, written for the Design A build; only the vector/
  text/icon family has size evidence from more than one shape in this
  session's own captures, and none of it pairs with a logged internal
  resolution (see that section's own caveats).
