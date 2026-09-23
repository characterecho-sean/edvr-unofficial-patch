# fix.hud_quality: the cockpit HUD at a higher target quality

## Status

BUILT, NOT FLOWN. `hud_quality_test.exe --self-test` covers the arithmetic;
no headset session yet.

State: `fix.hud_quality = off | 1.0 | 1.25`, on the Performance page,
generalises `advanced.surface_inflate` (fss_res.cpp) from a named `WxH` and
an integer 2..4 to a size learned from the interface-depth pass's own
classifier (`ui_depth.cpp`, gated on `fix.temporal_aa`) and a float factor
derived from the game's live HMD Quality. Viewports and (new)
scissor rects are rescaled at the draw-time backstop; copies/resolves onto a
tracked surface are detected and logged, not rescaled (see Mechanism).

Open: **gate G9** (`docs/crisp-ui-handoff.md:1017`) -- does inflating a
surface sharpen its TEXT, or only its vector lines. Never flown for any
inflation mechanism, named or matched. This ships specifically to run that
flight with a float factor tied to a real target (HMD Quality 1.0 or 1.25)
instead of an arbitrary named multiplier.

Next flight: HMD Quality 0.7 or lower, `fix.temporal_aa` on (a UI-swim fix
is not this doc's subject, but G9 asks to judge text, and DLSS is where the
text is soft to begin with), `fix.hud_quality = 1.0`. See "What the first
flight must show" below.

Ruled out: nothing yet -- unflown.

## The mechanism

`fss_res.cpp` already creates a named surface N times larger at
`CreateTexture2D` and rescales the game's viewport to match
(`advanced.surface_inflate`, integers 2..4, `fss_res.cpp:114`). This adds a
second way to reach the same code path:

1. **Which surfaces.** The interface-depth pass's classifier (`ui_depth.cpp`,
   gated on `fix.temporal_aa` -- there is no separate `fix.ui_depth` key;
   `config_test.cpp` asserts it stays absent) already learns, for its own
   purpose (giving UI draws depth under temporal AA),
   every render target a draw with vertex shader `666EF0C4C616F67E`
   (vector), `1012E00B3CB44469` (text) or `A3E5D3FCBC1165F8` (icons) lands
   in. `uiDepthLearnedSurfaceSizes()` (`ui_depth.h`) exposes that table's
   sizes (excluding the scanner's own chrome strip, a different shape
   learned at a second call site) to `fss_res.cpp`, so `fix.hud_quality`
   recognises the same three interface shaders without re-deriving the
   classifier. This is a real coupling: if `fix.temporal_aa` has never been
   on this session, the table is empty and nothing matches. The "on
   but no interface surface matched" log line covers both causes (the
   classifier saw nothing, or saw sizes that never matched) without trying
   to tell them apart.
2. **The factor.** `deviceHookHmdQuality()` reads the game's own
   `HMDRenderTargetMultiplier` from its newest `.fxcfg` (cached one second
   per thread). `factor = target / thatMultiplier`, applied only when it
   exceeds 1.0 by more than 1%, capped at 4 (the same ceiling
   `advanced.surface_inflate` uses, for the same reason). Unknown
   multiplier (no fxcfg, or no usable value in it) -- logged once, nothing
   changes. Read fresh at each candidate `CreateTexture2D`, not cached at
   ini-reload, because HMD Quality can change from Elite's own graphics
   menu without touching `edvr.ini`.
3. **Why the factor was integer-only, and what fractional needs.** The
   parser (`strtoul`) and the stored type (`uint32_t`) both refused a
   decimal point; the viewport paths already multiplied by a
   `static_cast<float>` of that integer, so the arithmetic was already
   float-shaped underneath. The change: `Spec::scale` and `Tracked::scale`
   are now `float`; a texture's Width/Height (which must land on an exact
   pixel count) round with `hudQualityRoundDim` (`v*factor + 0.5f`,
   truncate); viewport coordinates stay float and need no rounding, D3D11
   accepts them as given. `advanced.surface_inflate` itself still only
   accepts integers 2..4 -- its parser is unchanged on purpose, a hand-typed
   developer instrument keeping its own syntax -- it just now shares the
   float-capable storage and multiply code underneath.
4. **Viewports and scissors.** The existing `RSSetViewports` hook and its
   draw-time backstop (`fssResScaleDrawViewport`, `vscreen.cpp`) now
   multiply by a float rather than an integer scale, and take the target
   resource so a match-mode rescale can be counted separately. Scissor rects
   have no equivalent set-time hook -- nothing needed one before this -- so
   the draw-time backstop is their ONLY mechanism: it reads the current
   rasterizer state and scissor rect via `RSGetState`/`RSGetScissorRects`
   (no hook needed to call the GET side) and rescales only a rect that still
   spans the pre-inflation target exactly, leaving a genuinely narrower
   (clipping) scissor rect untouched rather than guessing at it.
5. **Copies and resolves.** `hookedCopyResource`, `hookedCopySubresourceRegion`
   and `hookedResolveSubresource` (already-existing hooks, used by several
   other features) now also call `fssResNoteCopyMaybeMismatched`, which logs
   (capped, 8 times) when either side of a copy is a tracked/inflated
   texture. **This does not rescale the copy** -- no such copy has ever been
   observed landing in one of these surfaces (the FSS body layer's own
   census found none either, `fss_res.h:46-49`), and box math for a shape
   nobody has seen would be a fix built on an untested hypothesis
   (AGENTS.md's own rule). It is a detector: the first flight's log says
   whether this ever happens at all, and if so, exactly what shape.

## Log lines

At the first surface resized, and every 30s after:

    hud quality: 1.0 (HMD Quality 0.70 -> factor 1.4286): interface
    surfaces resized 3 (vector 908x1361 -> 1297x1944, text 512x724 ->
    731x1034, icon 256x256 -> 366x366), viewports rescaled 42, scissors
    rescaled 0, copies touching one: 0.

A family not yet resized reads "vector none yet" rather than being omitted,
so the line always names all three. If the key is on and nothing has
matched after a minute:

    hud quality: on but no interface surface matched (the classifier saw
    none / the sizes did not match the learned fraction): nothing changed.

Distinguishing "the new code never ran" from "it ran and did nothing": the
configure-time line ("hud quality: 1.0. The cockpit's vector...") fires the
moment the key is read, whether or not anything ever matches; the two lines
above are the only ones gated on the feature actually doing something, and
between them cover both "never even tried" (wrong build, key not read) and
"tried, matched nothing" (classifier empty or sizes disagree).

## What the first flight must show

HMD Quality 0.7 or lower (Elite's own graphics options, not an EDVR
setting), `fix.temporal_aa` on, `fix.hud_quality = 1.0`:

1. The resize line above, naming all three surfaces -- vector, text and
   icon all matched and resized. If any reads "none yet" after opening the
   cockpit and visiting a panel that draws it, that family's classifier
   never saw it this session (check `fix.temporal_aa` is on).
2. The HUD text judged in the headset, at `off`, `1.0` and `1.25` in turn
   (a menu trip between each, since the surfaces only resize at the next
   creation) -- sharper or not. This is gate G9 itself: inflation has been
   measured reaching vector lines (the widget shader draws in a normalised
   space, so more pixels is more detail by construction); the glyph text
   samples a 2048x2048 A8 atlas whose own lifetime under inflation is
   unmeasured, and no inflation flight -- named or matched -- has ever been
   judged on the letters before this one.
3. Frame time unchanged (the target-indicator arc's own inflation flights
   measured no cost; a fixed-size surface a few hundred pixels larger is
   not where a frame budget goes).
4. **If the text is not sharper**, gate G9 fails and the lever beyond
   inflation is Design A in `docs/crisp-ui-handoff.md` (the UI-layer
   redirect, `## A5`) -- the extra texels this mechanism creates do not
   survive the render-resolution composite before EDVR's own upscale, and
   no factor or cap here changes that. This build does not attempt Design A;
   it is parked, per that doc's own status.

**Do not judge on the target direction indicator.** The
target-indicator-hunt arc (`docs/edvr-target-indicator-hunt.md` in memory;
see also `crisp-ui-handoff.md:875-879`) already ruled that artefact's sprite
is laid out at a fixed pixel size inside its surface and does not sharpen
however big the surface is -- it is the wrong instrument for this question,
however tempting to glance at since it is the easiest thing to find in the
headset.

## Contract

`fix.hud_quality` documented in `edvr.ini` under `[fix]` with a `# ui:` line
(Performance page, matching `fix.settlement_detail`'s precedent); read in
`fss_res.cpp`; asserted in `tools/config_test/config_test.cpp`. Contract
count +1 (one key, read and documented). `hud_quality_test.exe --self-test`
is a new rig in `build.bat`'s gate (auto-discovered by
`tools/run_jobs.py`'s `:rig_<label>` scan, the same as every other rig).
