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
- P2 motion prep (0.02-0.04 a frame each): skip the null ZS/ZUI loads in the depth-tile fill (probe.w bits; null-view loads cost 0.04 ms an
  eye per 9-tap input on this GPU, ui_resolve.h); 16x16 groups with a 20x20 tile. Needs an A/B harness for `mv` first (none drives it alone).
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
