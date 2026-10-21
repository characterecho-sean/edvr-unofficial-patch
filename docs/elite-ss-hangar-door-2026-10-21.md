# Elite drops hangar door pieces at Supersampling 0.5 (2026-10-21)

## Status

- **State (2026-10-21 13:00): the instrument flew once** (flight 1, log
  `edvr_gfx_20261021_123140`, v0.20.1-13-gdb52d367, compile-only, flat; see
  "Flight 1"). H0 is confirmed. Sean's report from the flight points to a
  load-time latch (H8), not to render-resolution LoD. The correction is on
  hold.
- **Symptom, revised by Sean in flight 1:** "now when I reload from the menu
  (regardless of SS) its missing, changing SS then fixes it." The morning's
  report (SS 0.5 missing, SS 1.0 present, also with EDVR suspended) fits that
  too if the game was loaded at 0.5 and then switched to 1.0. Station: MacLeod
  Market, 38 Lyncis (large Orbis hangar).
- **Render-resolution LoD (code, 334473):** every per-view LoD and cull test
  scales distance by the view's pixel size A = 2·tan(fovy/2)/H, H the view's
  target height. `0x2811E40` writes A, `0x281C5B0` copies it to the render
  views, and the part test `0x42B6C80` culls when A·(d−r)·s > t0. SS 0.5 halves
  every LoD and cull distance. Details: "Mechanism".
- **H0 measured:** the main view is slot 0 from caller `0x2010D0`, at the
  render size: 960×600 with A = 2.003296e-03 at SS 0.5, 1920×1200 with A =
  1.001648e-03 at SS 1.0 (cockpit), ratio 2.000.
- **Against H1/H2 for this symptom:** after both reloads at SS 1.0 (12:49:05,
  12:51:42) the main view's A was 1.001648e-03, the same as after the SS
  toggles (12:50:26). If the pieces were missing after a reload and back after
  a toggle, the LoD tests saw the same pixel size in both states.
- **Open hypotheses** (signatures under "Hypotheses"): **H8, a load-time
  latch** (leading): H8a per-object CPU state (a cached LoD level or visibility
  verdict), H8b GPU-side state sized at load. H1/H2 may still exist as a
  separate, smaller effect. H6, H7.
- **Census:** one capture (#1, 12:49:38, SS 1.0, after the reload). With no
  capture in a known present state, it names nothing yet. Its query brackets
  are event and timestamp only: no occlusion queries.
- **Ruled out:** see "Ruled out"; seven lines.
- **4.4.2.0:** no code change on the LoD path (IDENTICAL or RELOC-ONLY from
  332841). A data change to the door cannot be checked offline.
- **Correction:** built as the temporary key (factor 0.5000 applied at SS 0.5
  in flight 1). It addresses H1/H2 only. Do not ship it for this symptom unless
  the latch test leaves H1/H2 standing.
- **Next flight:** "Flight plan" (the latch test, two censuses at one SS).
  Temporary key: `advanced.detail_at_output_size` (off by default, flat only,
  live; off for the latch test).
- **Next offline step:** an SS change runs the sizing pass `0x284F190` (whether
  a reload does is not known). Besides the sizes it calls `0x2833D40` (scene,
  3, 256, 256), clears byte +0x1D of `*(*(scene+0xA8)+0x48)`, and calls
  `0x28AC2D0` and `0x28572D0`. Those, and the builder's visibility callback
  `0x288D280`, are where to look for a latch.
- **Not in scope, worth a look:** `advanced.texture_lod_bias = auto` derives
  the mip bias from HMDRenderTargetMultiplier in both profiles, so in flat it
  ignores SS.

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

The first plan's censuses: A = SS 0.5, key off; B = SS 0.5, key on; C = SS 1.0,
key off. The latch test's: M = after a reload (missing), P = after an SS toggle
back to the same SS (present). Same seat, head centred, no movement.

| | Hypothesis | Signature |
|---|---|---|
| H0 | prerequisite: the main view's H is the render height | **Confirmed in flight 1:** slot 0, caller `0x2010D0`, 960×600 / 1920×1200, A ratio 2.000 |
| H1 | door parts fail the table cull (f > t0) at render size | pieces return in B; diff(A→B) and diff(A→C) add the same door draws; A holds no door draws that C lacks |
| H2 | door parts take a coarser level that lacks them | pieces return in B; A also holds door-model draws absent from B and C (the coarse meshes: same shaders, other index counts) |
| H3 | screen-size term (sub-pixel parts) | restored draws are under 2 output px across; cannot be visible pieces (arithmetic above) |
| H6 | texture mips and alpha test at render size | B restores nothing; A and C hold the same door draws; a restart with `texture_lod_bias = -1` at SS 0.5 restores the pieces |
| H7 | another SS-dependent path (e.g. the builder's visibility callback `0x288D280`) | B restores nothing; C holds door draws A lacks; the mip bias run restores nothing |
| H8 | load-time latch: the door's visibility or LoD is decided at load and held until a render-size change | after a reload the pieces are missing at SS 1.0 as at 0.5, key off; an SS toggle either way brings them back; main-view A is the same in M and P (`lod view:`) |
| H8a | the latch is per-object CPU state (a cached LoD level or visibility verdict) | P holds door draws M lacks (diff M→P names them; same shaders, the door's index counts) |
| H8b | the latch is GPU-side state sized or filled at load | M and P hold the same draws; the pieces' pixels differ only |

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
- ruled out: the main view's H latched at a loading size, because slot 0 read
  the render size and the cockpit A within seconds of each reload (12:49:08,
  12:51:42); the 32×32 and 512×512 loading views are separate slots (flight 1).
- ruled out: D3D11 occlusion queries as the latch, because census #1's query
  brackets are event, timestamp and timestamp-disjoint only (t=0, 2, 3) over
  its two frames (flight 1, 12:49:38).

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

## Instrument (built 2026-10-21, flown once)

`src/d3d11/lod_view_hook.cpp` (glue), `src/d3d11/lod_view_core.h` (pure:
identity and pin, layout, factor, table, line text), rig `tools/lod_view_test`
(in build.bat's gate). Installed at device creation when the scope includes
flat; it acts only while the flat profile is active.

1. **The hook.** A CodeHook through a near relay on `0x2811E40`, pinned by the
   PE identity of 334473 and the sixteen prologue bytes (checked against the
   installed exe). After the original returns it reads, under SEH, the count at
   +0x15C00 (slot = count − 1), the descriptor's W×H and +0x26A byte, and the
   slot's A and B, and records them per (caller RVA, W×H). The caller is the
   return address the game's call pushed.
2. **Log lines.**
   - At install: `lod view: the scene-view builder (0x2811E40) is hooked,
     flat profile only ...`, or one `lod view: refused: ...` line naming
     why.
   - At the first config tick and on every change:
     `lod view: advanced.detail_at_output_size on|off [at start] (scene RxR
     output OxO, factor now F; ...)`.
   - Per (caller, size), when first seen and then every 5 s while called:
     `lod view: first|every 5s caller=0x... size=WxH slot=N primary=0|1
     A=... B=... calls=N | scene RxR output OxO key on|off factor F`. A and
     B are the game's own values, before the factor; factor is what the key
     applied to that view (1.0000 when nothing).
   - If nothing is built through the hook within 60 s of the first config
     tick: one `lod view: no scene view built ...` line. With more than 32
     (caller, size) pairs: one overflow line.
   - What would appear if the hook never ran: the install line and the key
     line, then the 60 s line and no view lines. A refused install shows its
     refusal and nothing else.
3. **`advanced.detail_at_output_size = off | on`** (temporary, live within a
   second, off by default, flat only, developer tier and hidden from the menu):
   for a view whose W×H equals the flat runtime's measured scene size
   (`flatRuntimeSceneSizes`), while that is below the swap chain's on both
   axes, A and B are multiplied by render height / output height.
4. **No new capture code.** The flat draw census (NumLock) and
   `tools/diff_draw_census.py` give A, B and C; the census lines' index counts
   and shaders separate H1 from H2.

Checked before commit: the rig passes under g++ and under the pinned MSVC (run
in Wine); `lod_view_hook.cpp` and `device_hook.cpp` compile clean at /W4 with
that MSVC; config contract, settings schema, exit paths, install reads, agent
material and Status block checks pass. The full build runs in CI.

## Flight plan

The latch test (H8), replacing the first plan. Key off, `texture_lod_bias = 0`,
the AA mode Sean uses (XeSS). The census key is Home (`hotkey.dump_draws`).
Same seat in the hangar, head centred, looking at the door.

1. Reload from the main menu at SS 1.0. Pieces missing? Home (census M1);
   screenshot.
2. Switch to SS 0.5. Pieces present? Home (census 0.5); screenshot. Present
   here, after a reload at 1.0, refutes H1/H2 for this symptom.
3. Switch back to SS 1.0. Pieces present? Home (census P1); screenshot.
4. Reload from the main menu at SS 0.5, and repeat 1-3 with 0.5 and 1.0
   swapped, if time allows (the morning's order).

Read: `python3 tools/edvr_log.py --target steam --expect-build <version>
--grep "lod view:|draw census"`; main-view A must match between M1 and P1. Then
`tools/diff_draw_census.py` M1→P1: draws only in P1 are the door (H8a); no
difference means H8b.

## Flight 1 (2026-10-21 12:31-12:56)

Build v0.20.1-13-gdb52d367 (this branch with main merged), compile-only, flat,
installed by the optimized-station session; log `edvr_gfx_20261021_123140.log`
(`edvr_log.py --expect-build v0.20.1-13-gdb52d367` matches). Read 2026-10-21 at
about 12:56, while the log was still growing.

- Install: `lod view: the scene-view builder (0x2811E40) is hooked` at
  12:31:40.682. Key off at start; on at 12:41:45.363 (factor 0.5000 at SS 0.5;
  the 5 s lines show factor 0.5000 on slot 0 from 12:41:50).
- Views: the main view is slot 0 from caller `0x2010D0` (the call in
  `0x200EC0`), at the render size throughout: 960×600, A 2.003296e-03 at SS
  0.5; 1920×1200, A 1.001648e-03 at SS 1.0 (cockpit; the menu camera reads
  1.979630e-03 and 9.898148e-04). Caller `0x28121EB` builds a copy of it in
  slot 1 and many small square views (96×96 to 109×109); the 32-entry table
  overflowed at 12:34:57 on those. Loading adds 32×32 and 512×512 views (slot 1
  from `0x2010D0`, slot 10 from `0x28121EB`), never in slot 0.
- Timeline (log and journal): SS 0.5 from launch; LoadGame 12:34:27; SS → 1.0
  at 12:48:30; to the menu 12:48:43, back in the hangar about 12:49:05 (from
  the resolve resets: a 4 s gap at the menu camera, then a 17 s gap); census #1
  at 12:49:38; SS → 0.5 at 12:50:12; SS → 1.0 at 12:50:25; to the menu 12:51:11
  and back at 12:51:42 at SS 1.0. The census key became Home at 12:43:01 (a
  live rebind; the capture's header still says "armed by NumLock").
- Sean: "now when I reload from the menu (regardless of SS) its missing,
  changing SS then fixes it." Whether the pieces came back when the key went on
  at 12:41:45 is not recorded.
- Main-view A after the reloads at SS 1.0 (12:49:08 on, 12:51:42 on):
  1.001648e-03, the same as after the toggles (12:50:31 on).
- Census #1: frame 57702, two frames, 2,136 offscreen draws, 116 copies, 123
  clear and query lines, nothing dropped. Queries: event, timestamp and
  timestamp-disjoint only. Without a partner capture in a present state it
  cannot name the door's draws.
