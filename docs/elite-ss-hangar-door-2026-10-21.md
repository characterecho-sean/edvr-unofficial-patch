# Elite drops hangar door pieces at Supersampling 0.5 (2026-10-21)

## Status

- **State (2026-10-21): offline reverse engineering done; nothing built,
  nothing flown.** Game build 334473 (4.4.2.0). Docs only.
- **Symptom:** flat, Steam on Proton, seated in a large Orbis hangar (MacLeod
  Market, 38 Lyncis, MarketID 3227880448) with the ship lowered: at SS 0.5
  several pieces at the bottom of the hangar door are not drawn; at SS 1.0 they
  are. Reproduced with EDVR suspended, so it is Elite's.
- **Mechanism (code, read in the 334473 Ghidra project):** every per-view LoD
  and cull test multiplies distance by the view's pixel size A =
  2·tan(fovy/2)/H, where H is the height of the view's target. The scene-view
  builder `0x2811E40` writes A (+0x530) and the orthographic term B (+0x540);
  `0x281C5B0` copies them to render view +0x550/+0x560. The part test
  `0x42B6C80` then rejects a part when f = A·(d−r)·s > t0, the first float of
  the model's LoD table, and picks its LoD level from the same f. The sizing
  pass `0x284F190` writes the render size as trunc(display × SS) per axis.
  Halving H doubles f, so at SS 0.5 every LoD switch and every table cull
  happens at half the distance it does at SS 1.0. Details: "Mechanism".
- **One link is inferred, not traced:** that the flat main view's H (camera
  object +0x864, set by `0x2874820`) is the render height and not the display
  height. Sean's SS repro supports it. The instrument's `lod view:` line
  settles it in the first second of the flight.
- **Code or data:** both. The resolution dependence is code. Which pieces
  vanish, and whether they vanish (t0) or swap to a coarser mesh (level), is
  per-model LoD table and mesh data.
- **4.4.2.0:** no code change on this path; every function maps IDENTICAL or
  RELOC-ONLY from 332841. A data change to the door's LoD tables cannot be
  checked offline (see "Did 4.4.2.0 change it").
- **Open hypotheses** (signatures under "Hypotheses"): H1 the door pieces fail
  the table cull (f > t0) at render size; H2 they pass but take a coarser level
  whose mesh lacks them; H6 texture mips and alpha test (GPU side, no LoD
  involved); H7 another SS-dependent path outside these tests.
- **Ruled out:** see "Ruled out"; five lines.
- **Feasibility:** yes, and cheap. A post-call hook on `0x2811E40` that scales
  A and B by render height / output height for views whose size is the render
  size makes the LoD tests see the output resolution. It belongs with EDVR's
  flat upscalers (XeSS, DLSS, FSR), whose render size is the game's SS ("game
  SS controls render size", log 20261021_102110). Cost: the draws SS 1.0 would
  issue for the same view. Risks under "Correction".
- **Recommendation:** fly the instrument below (one flight, one restart at
  most) before building the fix. If H1 or H2 is confirmed, ship the correction
  as the behaviour of the flat upscaled route, with no key.
- **Next flight:** "Flight plan". Temporary key it would add:
  `advanced.detail_at_output_size` (instrument only; remove when the arc
  closes).
- **Not in scope, worth a look:** `advanced.texture_lod_bias = auto` derives
  the mip bias from HMDRenderTargetMultiplier in both profiles
  (device_hook.cpp), so in flat it ignores SS, the same render-size question
  for textures.

## Symptom and environment

- Elite 4.4.2.0, build 334473, flat, Proton, Steam. Output 1920×1200;
  Settings.xml FOV 56.817; Custom.4.4.fxcfg LODDistanceScale 0.9,
  SSAAMultiplier 0.5 (read 2026-10-21 10:36).
- EDVR's log `edvr_gfx_20261021_102110.log` (v0.20.1-6-g652cc5d0): flat route
  `xess-upscale R=960x600 E=1920x1200`; the optimized station seal at 10:23:09
  names pad LandingPadLargeA, shell HangerLarge, the bay records
  HangarGate_Ramp1_0/1_1/2_0/2_1, HangarGate_SafetyHC, HangarGate_Base, and a
  box 446 × 148 × 556 m with BlastShieldBase_B about 390 m along the shaft from
  the lowered pad. Hangar geometry spans hundreds of metres.
- Journal: the Docked event at MacLeod Market is in
  `Journal.2026-10-21T094332.01.log`; the 10:21 and 10:32 sessions start docked
  there.

## Mechanism

All addresses are RVAs in build 334473 (image base 0x140000000).

1. **The pixel size.** `0x2811E40` (scene-view builder, four callers) builds
   slot `count` of a 0x570-byte scene-view array:
   - `fVar9 = 1.0 / H`, with H = descriptor +0x264 (u32; constant at
     `0x4DE165C` = 1.0);
   - `fVar10 = 2.0 / P22` (constant at `0x4E328D0` = 2.0; P is the camera's
     projection at camera +0x1B0, P22 at +0x1C4);
   - +0x530 = P34·fVar10·fVar9 = A; +0x540 = P44·fVar10·fVar9 = B. For a
     perspective camera P34 = 1 and P44 = 0, so A = 2·tan(fovy/2)/H, the
     world size of one target pixel at unit distance, and B = 0.
2. **The descriptor's H.** The main caller `0x200EC0` copies the camera
   object's +0x860 qword (width, height) into descriptor +0x260/+0x264. The
   camera's setter `0x2874820` stores its size argument at +0x860. Of its
   callers, the virtual method `0x2874170` passes the scene's view object
   +0x310 (`*(*(ctx+0x2CB8)+0xA8)+0x310`, ctx the render context); that it is
   the flat main camera's path is likely, not proven. The other callers were
   not traced: `0x287A5B0` (a three-view loop with sizes from its own table),
   `0x287D660` (six calls), `0x28457F0` (ten calls), `0x287B110`, `0x2889A00`,
   and two that pass a size of 0.
3. **The render size.** The sizing pass `0x284F190` copies the menu's SS
   (ctx+0x3564) to ctx+0x359C and writes its size record with display size at
   +0x20/+0x24 and render size trunc(display × SS) at +0x30/+0x34. At SS 0.5 on
   1920×1200 that is 960×600, which XeSS's input confirms. Not traced: the hop
   from that record to view +0x310 (step 2). It is the inferred link named in
   Status.
4. **The copy.** `0x281C5B0` copies scene +0x530 → render view +0x550 (A) and
   scene +0x540 → +0x560 (B); render view +0x540 is the camera position. Render
   views are 0x6A0 bytes at ctx+0x40+i·0x6A0.
5. **The tests.** All four per-view admission tests read render view
   +0x540/+0x550/+0x560 (design-occlusion-culling-2026-09-22.md, close-out 1;
   addresses moved to 334473 per build_diff):
   - active mask `0x4334000` (per-view distance and LoD, no planes);
   - record test `0x430B7F0` (per-view distance and screen size, writes the
     record's LoD nibble or clears the view bit);
   - per-(record, view) gate `0x4311CA0`;
   - per-part test `0x42B6C80`, decompiled here:
     - screen size: pass iff 0.5·(A·d + B) ≤ r (constants `0x4E328B0` 0.5,
       `0x4E328C0` 1.0);
     - frustum `0x4F4F70`;
     - table: f = A·(d − r)·s + B, s = ctx+0x30 = 2 − LODDistanceScale
       (`0x281C3D0`, unchanged); reject when t0 < f; else the level is the
       first i with t[i+1] ≥ f, up to the table's count at +0x70.
   d is the engine's rsqrtps(rcpps(|c − cam|²)).

Worked numbers for Sean's settings (FOV 56.8° taken as vertical, s = 1.1):

| SS | H | A | f at 300 m | screen-size floor r at 300 m |
|---|---|---|---|---|
| 1.0 | 1200 | 9.01e-4 | 0.297 | 0.135 m |
| 0.5 | 600 | 1.80e-3 | 0.595 | 0.270 m |

A door piece whose table has 0.297 < t0 < 0.595 at that range is drawn at 1.0
and culled at 0.5. The game's LoD slider cannot buy it back: s has its floor at
1.0 (LODDistanceScale 1.0), worth 10% here against the factor 2.

## Code or data

The dependence on render size is code: f uses the render target's pixel size,
by design, in every LoD and cull test. Which parts are affected is data: each
model's 0x80-byte LoD table (t0 and the level thresholds) and the mesh set each
level draws. A part with a small t0 vanishes outright (H1); a model whose
coarser level omits the part loses it by substitution (H2).

## Did 4.4.2.0 change it

- Code: no. `analysis/build_map_332841_to_334473.csv` maps the whole chain
  IDENTICAL or RELOC-ONLY: scene-view builder 0x280F800→0x2811E40, copy
  0x2819F70→0x281C5B0, s setter 0x2819D90→0x281C3D0, part test
  0x42B3FC0→0x42B6C80, record test 0x4308B30→0x430B7F0, gate
  0x430EFE0→0x4311CA0, active mask 0x4331300→0x4334000, traversal
  0x4312040→0x4314D00, builder 0x42B4420→0x42B70E0, sizing pass
  0x284CB70→0x284F190, settings copy 0x281C0D0→0x281E710, fxcfg loader
  0x2855A50→0x2858070.
- Data: unknown. The update rewrote 848 files under `Win64` (0.50 GB of 57.2
  GB). No data file holds the names HangarGate_Ramp or HangerLarge in plain
  text (grep over all 102,960 files), and the pre-update files are gone, so the
  door's tables cannot be compared offline. 4.4.2.0's LoD notes (megaship
  vents, Powerplay decals) do not name stations.
- So the effect has existed at least since 332841 for any model whose table
  straddles the 2× change; Sean's sense that it is new is either a data change
  to these doors or the first look at this view at SS 0.5.

## Hypotheses

Census A = SS 0.5, key off; B = SS 0.5, key on; C = SS 1.0, key off. Same seat,
head centred, no movement between them.

| | Hypothesis | Signature |
|---|---|---|
| H0 | prerequisite: the main view's H is the render height | `lod view:` main slot H = 600 at SS 0.5, 1200 at 1.0, A ratio 2.000 ± 0.1%. If H = 1200 at both, H1/H2 are refuted together |
| H1 | door parts fail the table cull (f > t0) at render size | pieces return in B; diff(A→B) and diff(A→C) add the same door draws; A holds no door draws that C lacks |
| H2 | door parts take a coarser level that lacks them | pieces return in B; A also holds door-model draws absent from B and C (the coarse meshes: same shaders, other index counts) |
| H3 | screen-size term (sub-pixel parts) | restored draws are under 2 output px across; cannot be visible pieces (arithmetic above) |
| H6 | texture mips and alpha test at render size | B restores nothing; A and C hold the same door draws; a restart with `texture_lod_bias = -1` at SS 0.5 restores the pieces |
| H7 | another SS-dependent path (e.g. the builder's visibility callback `0x288D280`) | B restores nothing; C holds door draws A lacks; the mip bias run restores nothing |

H1 and H2 take the same correction; telling them apart answers only the
code-or-data question for this model.

## Ruled out

- ruled out: an EDVR cause, because Sean reproduced it with EDVR suspended
  (`install_edvr.py --suspend`, 2026-10-21), and Optimized stations and AA were
  ruled out before that.
- ruled out: a 4.4.2.0 code change on the LoD path, because every function on
  it maps IDENTICAL or RELOC-ONLY from 332841 (list above).
- ruled out: the screen-size term as the cause of visible missing pieces,
  because it removes only parts under one render pixel across (two output
  pixels at SS 0.5); H3 stays listed only to be read off the census.
- ruled out: LODDistanceScale as a workaround, because s cannot go below 1.0,
  and SS 0.5 needs s × 0.5.
- ruled out: comparing the door's data across the update offline, because the
  asset names are not plain text in any data file and the old files were
  replaced.

## Correction

**Where:** a post-call relay on `0x2811E40` (prologue `48 89 5C 24 18 55 56 57
48 83 EC 30 44 8B 81 00`). After the original returns, the slot it wrote is
`this + (count − 1)·0x570`, count at this+0x15C00; the descriptor (rdx) gives
W, H at +0x260/+0x264. When the flat upscaled route is live with render R and
output E, R ≠ E, and the descriptor's W×H equals R, multiply slot +0x530 and
+0x540 by R.h / E.h. Everything downstream (the copy and all four tests) then
evaluates at the output's pixel size. No engine code is patched beyond the
relay.

**Cost:** the main view issues the geometry SS 1.0 would issue; pixel cost is
unchanged. In the settlement measurement the comparable LoD-distance factors
cost 1.9% (×1.875) to 6.7% (×2.25) of eye draws, and LoD level shifts were
draw-neutral (design-settlement-lod-bias-2026-09-22.md §§1-5). Stations are
unmeasured; the census pair A/C prices it.

**What it could break:**
- Parts between one and two output pixels across come back (the screen-size
  term). Under XeSS, DLSS or FSR that is detail the reconstruction is built
  for. Under the game's own bilinear SS < 1 it would shimmer, which is why the
  correction belongs to EDVR's upscaled route and not to SS < 1 in general.
- Views that coincide with the render size but are not the main view
  (render-to-texture panels at the same size) would be rescaled too. The
  instrument logs every slot's W×H and caller, so the census flight shows
  whether any exist.
- Shadow views keep their own H (shadow map size) and are untouched, so a
  part's shadow can use a coarser level than the part. That is already true at
  SS 1.0.
- Readers of A and B beyond the four tests are not proven absent. A
  displacement scan for +0x550 finds 100 functions, mostly unrelated structs;
  the five in the traversal range are not view LoD tests. Views are also
  reached by computed index, which such a scan misses.
- VR: EDVR holds SS at 1.0 (vr-supersampling-gate-2026-10-10.md), so the factor
  is 1. An eye-level analogue (render vs swapchain) is a separate question.

## Instrument (designed, not built)

1. **`lod view:` lines.** The relay above, observe only: per distinct (caller
   return address, W×H) the first time and then every 5 s, log slot index, W×H,
   A, B, +0x26A flag, and EDVR's current R and E. The main-scene caller is
   `0x200EC0` (return address in that function). If the relay never runs, no
   `lod view:` line appears; a startup line names the relay's install result so
   absence is unambiguous.
2. **`advanced.detail_at_output_size = off | on`** (temporary, live, off by
   default, flat only): applies the correction above. A change logs one line
   with the factor.
3. **No new capture code.** The flat draw census (NumLock) and
   `tools/diff_draw_census.py` give A, B and C; the census lines' index counts
   and shaders separate H1 from H2.

## Flight plan

Commit before compiling, then the compile-only path in AGENTS.md. Keep
`texture_lod_bias = 0` and the AA mode Sean used (XeSS).

1. Dock at MacLeod Market (or any large Orbis pad), lower into the hangar, look
   at the door with the head centred. SS 0.5, key off: confirm the pieces are
   missing; NumLock (census A); screenshot.
2. Set the key on in `edvr-flat.ini` (live); wait 2 s; NumLock (census B);
   screenshot. Say whether the pieces came back.
3. Key off, SS 1.0 in the menu, same seat: NumLock (census C); screenshot.
4. Only if B restored nothing: quit, set `texture_lod_bias = -1`, relaunch,
   repeat step 1 (H6).

Read: `python3 tools/edvr_log.py --target steam --expect-build HEAD
--grep "lod view:|detail_at_output"`, then the census diffs A→B, A→C and
C→A. H0 first.
