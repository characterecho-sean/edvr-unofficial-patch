# EDVR's non-DLSS GPU work: where it goes and what was trimmed (2026-10-14)

## Status

State: BUILT, NOT FLOWN. Branch claude/edvr-gpu-trims, cut from origin/main 498e4b16. Two cuts, each its own commit, no config
key: output identical by construction and held by rigs. Expected saving together 0.06-0.09 ms a frame (1.2-1.8% of EDVR's 5.07).
Environment: RTX 5090, SteamVR OpenXR, eye 2016^2 -> DLSS 4032^2, preset K, UI quality 100, hangar, Frontier log edvr_gfx_20261014_073440
(07:39:41 census, v0.19.1-204-g1a452fe8). NVIDIA reports no PSSpecifiedStencilRef: the seed's stencil is the per-bit fallback (mask 00 here).

| item (census name) | ms/frame | what it does, why it costs that | file:line |
|---|---|---|---|
| motion prep | 0.374 | one `mv` dispatch per eye, 63,504 groups of 8x8 over 2016^2, 12x12 depth tile (3 loads a tile texel: Z, ZS, ZUI), ES per pixel; ~100 MB an eye, 0.06 ms by bytes: latency/ALU-bound | temporal_pass.cpp:4164, temporal_shader_source.h:826 |
| UI resolve | 0.292 | dlOut -> dlSubmit over 4032^2: ~0.075 passthrough bytes + corona hold ~0.037 + mask fetches ~0.032 (bench, per eye) | temporal_pass.cpp:3845, ui_resolve.h |
| sharpen | 0.155 | RCAS 16x16, 4032^2 read + write; at its bytes already | sharpen_pass.cpp:669 |
| menu | 0.019 | one dispatch over the panel box, only when the EDVR panel shows (the full-frame region copy before it is outside the figure) | menu_panel.cpp:1400 |
| UI layer composite | 0.164 | in place, 32x8, skipped when the eye has no layer draws (ui_layer.cpp:4811); reads the 65 MB layer, writes covered texels: near its bytes | ui_layer.cpp:2088 |
| engine velocity | 0.162 | six spans, not dispatches: 2 snapshots (1.4 MB pool copy + scene CB), 2 eye clears, the skin join (1 group, 0.040) and pose table (6 dependent dispatches, 0.039) | engine_velocity.cpp:1307,1775; skin_join_gpu.cpp:392,490 |
| UI layer reissues | 0.212 | 66.3 scopes: 30.3 write-backs (a HUD draw re-issued colourless so its stencil lands in the game's buffer, ~3 us each) + 2 tonemap re-issues (0.062 an eye, plus the coverage pass 0.036); 34.1 "empty" scopes issue nothing | vscreen.cpp:3748,3908; ui_layer.cpp:3604,3609 |
| HDR HUD seed | 0.291 | per eye: copy of the game's 2016^2 D32S8 (32 MB), stencil clear, one fullscreen depth-export draw into 4032^2 D32S8 (130 MB) | ui_layer.cpp:1212, ui_layer_seed.h:79 |

Seed on the RTX 5090 (`ui_layer_seed_test --bench-seed`, the production Seeder, 2016^2 -> 4032^2, mask 00 + depth, median of 301): copy alone 0.0245,
seed from a copy 0.0947, seed in place 0.0665. Scratch variants (not kept): stencil clear 0.0005, the same fetch to an R32F colour target 0.0337
(depth export is 2x the colour export), D32 target instead of D32S8 0.0676 (no gain), one stencil-bit pass 0.0176. In flight 0.146 a seed.

Built (a commit each):
1. Seed reads the game's depth texture where it lies (no CopyResource). Elite's is R32G8X24_TYPELESS, bind 0x48 (depth + SRV), so views
   over it work (two kept an eye, so a game that alternates two textures does not remake them); a texture that refuses them keeps the copy.
   Same bytes at the same point in the stream. Proof: ui_layer_seed_test
   `inPlaceCase`: seed from the source vs seed from a CopyResource copy, byte for byte, 4 formats x 30 mode/value cases, WARP and the
   5090, the game's depth view still bound on the immediate context, and the D3D11 debug layer must stay silent (a control list that
   does bind SRV + DSV together must be reported). Expected: -0.028 a seed, seed 0.146 -> ~0.118, item 0.291 -> ~0.235.
2. UI resolve's corona hold fetches the centre texel first and skips the other twelve when its luma is at or above the limit (then
   the weight is exactly zero). ui_holo_pass_test: 839 checks byte for byte against the frozen pass, 269 fixtures. Bench (4032x3896,
   mixed content): idle 0.1499 -> 0.1413 an eye; menu row unchanged within noise. Content-dependent: 0 on all-dark sky, 0.037 an eye
   at best. Expected item 0.292 -> 0.275-0.290. Pin re-made: temporal_shader_build.cpp "UI resolve" 0xC2BDB42B78A27FB5 -> 0x49E58C9F931CD699.

Answers: seed in D24S8/D32: no gain (measured) and it changes the depth the HUD tests against. At 2016^2 and upscaled: the layer's draws test
a 4032^2 depth, so edges change. Shared between eyes: no, depth is per eye. Skipped on an empty layer: already (first depth-testing HUD draw
only). Empty reissue scopes issue nothing (GPU 0; only the census scope opens). Engine velocity: no dispatches to batch (see table).

Proposed, not built (ranked by saving; all change the data flow and want a flight, or are tiny):
- P1 UI resolve passthrough (up to ~0.07 an eye, 0.15 a frame): let DLSS write dlSubmit directly and resolve in place only where it
  changes a pixel. Needs NGX to accept the typeless dlSubmit, and the eye-run captures/probeNv that read dlOut after the resolve.
- P2 motion prep: null ZS/ZUI skip BUILT 2026-10-23, not flown (G09, RocketsProjects' finding): mv -13% both layers absent, -6% UI
  depth bound; native TAA's 3x3 depth a group tile, -13%, filled only when read (journal). Flight: "motion prep depth layers" line.
- P3 HUD tonemap re-issue + coverage (0.098 an eye, 0.2 a frame): two passes over the 130 MB HDR layer; merging needs the game's tonemap PS to
  write alpha. Skipping untouched tiles needs a content test the layer does not have (same as the composite's "open, not tried").
- P4 pose table: six dependent dispatches, ~0.04 ms; fusing two or three saves ~0.02. Join: one group, latency-bound.
- P5 UI resolve mask fetches as a group tile (~0.005 an eye per mask), the hold's shared neighbourhood (~0.01 an eye).
- P6 census: do not open reissue scopes that issue nothing (CPU only, and it removes the "34 empty" evidence).

Ruled out: per-draw route timestamp pairs as a GPU cost, because a 5090 microbench of 32-64 blended draws gives +0.03-0.1 us a draw with a pair
around each; a stencil-less D32 layer target for mask-00 seeds, because the draw costs the same (0.0676 vs 0.0666); thread-group shapes for
composite/RCAS/UI resolve (ui-layer-2026-09-23.md, already taken or ruled out).

Flight check (edvr_log.py --expect-build HEAD first; compare to the 07:39:41 line: seed 0.291 (2.00/frame), UI resolve 0.292, in-frame 0.665, EDVR 5.073):
- gfx log: once "ui quality: layer: the depth-stencil seed reads the game's own 2016x2016 D32_FLOAT_S8X24_UINT texture in place". If the log says
  "refused shader views" instead, cut 1 is inert on that rig and the copy still runs.
- the target-creation line says "reading the game's own in place, no copy of it".
- census "HDR HUD depth-stencil seed" ~0.235 (2.00/frame); the detail line "ms a seed" ~0.118 (was 0.146).
- census "UI resolve" 0.275-0.290 (content-dependent); everything else within noise; EDVR ~5.00.

## Journal

2026-10-14. Read order: gpu_census.cpp (item names, sampling: K=8 timed calls a turn, null-pair corrected), then each call site above.
Census sampling note: "UI layer reissues" is a mean over timed occurrences (empties included) times the occurrence count, so the 34 empties
dilute the mean and add nothing; the sum is unbiased.
Seed bench method: 2016^2 R32G8X24_TYPELESS (DEPTH_STENCIL|SRV) filled by a draw, 4032^2 D32S8 target, the production Seeder, jitter (0.25, -0.4), 8
back-to-back repeats between two timestamps in one disjoint scope, median of 301, no debug layer. The scratch variants used the Seeder's PS text.
The timestamp-pair control: 32 draws covering 20% of a 2016^2 target, blended; none 0.0565 ms, a pair around each 0.0576, around every 8th 0.0566.

2026-10-23. P2's first half BUILT (G09), branch claude/perf-g09-zscene-gate from origin/main c16a46b3, no config key. The
investigation (the audit finding, a no-smoke A/B bench on an RTX 5060, the presence census idea) is RocketsProjects'. zSceneAt
(temporal_shader_source.h) fetches t6 (the smoke's depth) only while probe.w bit 64 is clear and t7 (the private UI depth) only while bit
256 is clear; the absent operand is the +0.0 a null view returns, so the max chain is unchanged. temporal_pass.cpp sets the bits from the
SRV array each dispatch binds (mv and main), after it is built, then writes the constants. Clear bit = the old fetch. The eye dump's
uiFlags word now carries the two bits too. New log line beside "UI resolve totals": "motion prep depth layers" (mv dispatches by which
layers were bound). DXBC (fxc /Fc of the four variants): every t6/t7 load sits inside an if_z block (6 in mv, 24 in aa fast, 64 in aa).
fxc writes the max operands in the other order (max(zs, z) where the old shader had max(Z, ZS)); the parity below includes both signed
zeros and passes on WARP and the 5090, so neither implementation's max depends on operand order there.
Proof: ui_depth_test (zscene_gate_test.h) compiles the production zSceneAt beside the old expression, Z/ZS/ZUI = every triple of
{+0, -0, 0x1, 0x7FFFFF, qNaN, -qNaN, +Inf, -1, 0.5, 1}, all four presence combinations, bits matched and clear, screen bit 32 off and
on: asuint-equal on all 20,000 texels, WARP (build) and RTX 5090 (`--zscene-gate --adapter nvidia`). Controls: bit 64/256 over a bound
layer reads exactly as that view null and differs where the layer wins. screen_motion_test: the five shipping blobs, t6/t7 null in three
combinations, byte-identical across MV/ZC/MK/DT/O/N/Stats with the bits clear vs set; a bound, nearer UI depth with bit 256 set changes
both entries' outputs (a blob built before the gate would pass the parity vacuously).
Bench (`ui_depth_test --zscene-gate --adapter nvidia --bench`; method and fixture adapted from RocketsProjects' no-smoke A/B: one
eye 2016^2, temporal_mv_fast_cs, blocks of 48 dispatches, 12 ABBA/BAAB paired rounds, three runs; changed: all blocks queued before the
clocks are read, since draining after each 2.5 ms block let the 5090's clocks sag; every output byte checked against (a) first, and the
no-ZS variant must differ with the smoke bound). Arms against (a) the shader before the gate: (b) the gate, bits set; (c) a compiled
no-ZS variant (RocketsProjects' max(max(Z, 0), ZUI); bench-only); (c') the same without ZUI; (d) the gate, bits clear, smoke bound.
Fastest block (ms per eye; identical in all three runs to +-0.0005), and the paired median saving's range over the runs:
  UI depth off: (a) 0.0426-0.0447; (b) 0.0370 (-13%, paired -6..-17%, 10-12/12 rounds); (c) 0.0398-0.0405 (-9%); (c') 0.0355-0.0359
  (-20%); (d) -1.5% (paired +0.8..+1.4%, 6-7/12: no regression).
  UI depth on: (b) 0.0404-0.0409 (-6%, 7-10/12); (c) 0.0408-0.0410 (-5%); (d) all bound -1.1% (6-8/12: no regression).
So the uniform branch takes all of (c)'s saving (more with the UI depth off, where it skips ZUI too) and costs nothing bound; against
the fully compiled-out (c') it leaves ~0.0013 ms an eye. Shipped as the branch: the two-copy switch is not needed. The first runs that
afternoon (Elite running on the same GPU at 38%) moved by +-40% between runs and are not evidence. RocketsProjects' RTX 5060 figures
for (c): 0.3104 -> 0.2451 UI off, 0.2584 -> 0.2300 UI on (72/72 rounds).

2026-10-23, later. The native-TAA own pass (`main`) read the 3x3 of zSceneAt around every pixel in fetchHistoryT, 9 calls a pixel and
twice that again for the instrument's two depth candidates (diagnostic shader). It now reads a 10x10 groupshared tile (the 8x8 group,
a one-texel apron, clamped exactly as the 3x3 clamped), filled at the top of main by every group before one barrier in uniform control
flow; a static flag main sets selects the tile, so any other entry that calls fetchHistoryT (a rig's) fetches as before. Unconditional
fill (also with no depth bound or no history: a few null loads then, never a stale entry). EDVR_MAIN_DEPTH_TILE 0 compiles the old
3x3; fxc gives it the bytecode of the shipped main byte for byte, both variants, so the reference is the old shader, not a rebuild.
DXBC, fast main: Z loads 12 -> 4 sites (the 3x3 gone, the fill's loop, count[17] and holoPixel kept), one tgsm array of 100, one barrier.
Proof: ui_depth_test (main_depth_tile_test.h) holds both embedded main blobs to the EDVR_MAIN_DEPTH_TILE 0 compile, O/N/UN/Stats byte for
byte, over 50 configurations on a 37x29 region offset (5,3) in 48x40 (partial groups both ways; depth on/off, head depth term, world
path, six probe.w words incl. screen, scanner, adaptive UI and absent layers; no history), WARP in the build and the 5090 by hand
(`--main-tile --adapter nvidia`); a mutant tile index (every tap the centre) is caught in exactly the 18 configurations where the 3x3
runs. The terrain rig's anchors in fetchHistoryT are untouched. Bench (`--main-tile --adapter nvidia --bench`, the paired method above,
temporal_aa_fast_cs 2016^2, depth on, world path on): bound 0.1440-0.1481 -> 0.1263-0.1299 ms an eye (paired -0.019..-0.021, -13 to
-14%, 12/12 rounds in all three runs); layers absent with the bits set 0.1380 -> 0.1186 (-0.018..-0.021, 12/12).

2026-10-23, review of G09 (adversarial). Five findings, fixed:
1. The CPU half was unpinned. Both GPU rigs compute their own bits, so a slot slip (srvs[5], always null) would set bit 64 on every
   dispatch and skip a bound smoke depth with every rig green. depthLayersAbsentBits is now inline in temporal_pass.h, and
   ui_depth_test's cpuHalf (zscene_gate_test.h) checks it over the four t6/t7 combinations with a distinct non-null pointer in every
   other slot, and each other slot nulled alone. It also scans both dispatches in temporal_pass.cpp: the bits come off the array that
   the last CSSetShaderResources(0, ...) before that Dispatch binds, are OR'd into p.probe[3] before setParams, and setParams precedes
   the Dispatch. Mutants caught: srvs[5]; setParams before the OR (main's site and mv's); bits off nullSrvM (mv).
2. The tile fill ran in every group, also on history resets and with no depth, where the 3x3 never runs. It is now gated on
   haveHistory && knobs.y && (tvUsed.w || (tvCam.w && split.x > 0)), plus candMask & 10 in the diagnostic shader: constants only,
   with the barrier left unconditional. DXBC, both variants: the g0 store sits inside if_nz, and sync_g_t follows its endif at depth
   0 (tileFlow in the rig pins this). EDVR_MAIN_DEPTH_TILE 0 still compiles to ab78bbf4's main byte for byte, both variants. The A/B
   poisons the tile before its gate checks. Unpoisoned, a wrong gate passed on the 5090, because an unfilled entry read the identical
   tile left by the dispatch before. With the poison, the production gate matches in all 50 configurations, both variants, on WARP
   and the 5090, and a gate without the world split is caught in exactly its 6. Bench (`--main-tile --adapter nvidia --bench`,
   2016^2, temporal_aa_fast_cs, three runs; ungated = 42bf0a99's fill): history reset 0.0585-0.0591 -> 0.0341-0.0384 ms an eye
   (paired -0.021..-0.023, 12/12; fastest 0.052-0.053 -> 0.034); no depth 0.1277-0.1281 -> 0.0877-0.0942 (paired -0.033..-0.043,
   12/12; fastest 0.111-0.117 -> 0.080-0.085). Gated against untiled in both: -0.002..+0.003, noise. With depth on, the tile still saves paired 0.013-0.025 against untiled.
3. main_depth_tile_test.h claimed origin/main's bytecode as the reference; it is ab78bbf4's (origin/main predates that zSceneAt).
4. screen_motion_test's production-blob parity had no -0, NaN, denormal or negative depths. It now also runs with -0, +0, both
   quiet NaNs, denormals of both signs, -1 and -0.01 in Z and in the bound layer: byte-identical, WARP (the rig has no hardware path).
5. probe.w bit 256 was per-object motion's until 319c1c5a; the cbuffer comment says so, for eye dumps older than that.
on_foot_split_test's R5k pin now counts four `split.x > 0.0` (the fill's gate is the fourth).

2026-10-23, final adversarial review: three small findings, fixed.
1. main_depth_tile_test.h never varied split.x or candMask (always 2 and 15), so a gate that under-filled at split.x 0, or tested only
   candidate 1 (candMask & 2) and dropped candidate 3, passed. It now runs 78 configurations: the 50 above, 16 with split.x 0 (head term
   and world term each on or off, probe.w 0 and 128) and 12 on the candidate mask: 0, 2, 8, 5 and 10, each with the split on and the
   world term off and with the world term set and the split off, plus two with candidates 1 and 3 asked and no depth or no history. The output A/B
   cannot see an OVER-fill (a gate that fills where nothing reads the tile writes the same bytes), so withFillProbe counts the groups
   that wrote the tile (Stats[120], rig-only text on the poisoned compile) and the rig requires all 20 where the 3x3 can run and none
   where it cannot, per variant. Mutants of the production gate, each failing the rig: `tvUsed.w || tvCam.w` (the split.x guard
   dropped: over-fill at split.x 0, caught by the probe, after the output compare for that configuration had passed), `(tvUsed.w || tvCam.w)
   && split.x > 0` (under-fill at split.x 0 with the head's term, caught by the A/B), `candMask & 2` and `candMask & 8` (candidate 3 or 1
   dropped, caught by the A/B at candMask 8 and 2), `candMask != 0` (candidates 0 and 2 cannot read the tile; caught by the probe at
   candMask 5). The mutant tile index is now caught in 22 configurations, the world-split-less gate in 6.
2. screen_consumer_test's special-depth pass compares the same new blob with a null-view fetch against the literal +0.0 through the same
   max, so it proves skipping equals fetching for -0/NaN/denormal/negative depths, not that the new max operand order matches the old
   shader; its comment now says so and points at zscene_gate_test.h, whose parity runs the production zSceneAt against the old expression.
3. logDsvMemo ran every 30 s in the flat profile with the layer off, when flatUiLayerNoteSceneDraw returns early and nothing can reach
   the memo. The flat call is now gated on the layer being live (or the window having counted something before it stood down); a live
   layer that nothing reached still logs its zeros. ui_after_ui_test's CPU-06 scan pins the new form.
