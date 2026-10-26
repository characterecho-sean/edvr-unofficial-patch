# Contributor performance audit EDVR-001: review and changes (2026-10-23)

All of the investigation behind this arc is RocketsProjects' work: the EDVR-001 source audit (42
findings, each with its side effects and invariants), the no-smoke A/B bench, and the smoke-presence
census design. This doc records what EDVR's review made of it and what was built from it.

## Status

- **State:** RocketsProjects' read-only audit (EDVR-001, 42 findings, source pinned at 74081399,
  received 2026-10-22) was checked against HEAD c16a46b3. The 23 active findings plus the smoke-depth
  reads (G09) each got a source review. Eight became changes on branch
  `claude/edvr-performance-optimizations-8aff5f`. All of them keep output identical. First flown
  2026-10-23 (below).
- **Built (CPU):** CPU-02, CPU-04, CPU-05 and CPU-06 in 92904483; CPU-12 and CPU-17 in 24a0c1b0.
- **Built (GPU):** G09 (ZS/ZUI absent bits in `zSceneAt`) and a tiled 3×3 depth fetch in the
  native-TAA pass, in ab78bbf4, 42bf0a99 and fa1afa36; see the G09 entry below.
- **Reviewed:** an adversarial review of every commit found no output-changing bug. Its test-strength
  and diagnostic findings were fixed in 1088ed18 and fa1afa36.
- **No config keys.** Every change is exactly equivalent, so none gets a toggle.
- **Flight 1** (2026-10-23, v0.21.0-19-g02c8f39c, Frontier).
  - **Flights:** 133549 and 133813 on Pimax (menu and hangar); 135250 on SteamVR aapvr (menu,
    hangar, concourse on foot, Explorer Cam switches).
  - **G09 presence (135250, 69,144 mv dispatches):**
    - neither layer bound: 58%;
    - UI depth only: 27%;
    - both: 15%;
    - smoke only: 0.
    So the ZS load is skipped on 85% of dispatches and the ZUI load on 58%.
  - **CPU-06:** the depth view memo hits 96-99.5%.
  - **G09 timing is below the noise of a free flight.** The temporal price mv line is flat (0.10
    against 0.11 ms a pair). The census "motion prep" door reads 0.17-0.36 ms a frame in new menu
    windows against 0.13-0.21 in baselines, where the menu ran different content (composite and
    hologram passes at 2/frame against about 1). An exact reading needs a same-scene A/B.
  - **The main-menu 3D hangar hitches are game-side and in every build**, at the same size:
    - the scene creation burst (139 textures, 121 buffers, 208.1 MB);
    - a single 450-520 ms freeze at Elite+0x5d6e7f with no EDVR code on the stack;
    - 30-60 ms frames with EDVR under 1 ms in the frame.
  - **The concourse overrun past 11.1 ms is not a regression.** It is a busy-area CPU limit that
    starts at an area-streaming frame, not at a mode switch. Explorer Cam adds about 1.5 ms. The
    10-14 flight (v0.19.1) shows the same state and the same switch transients.
  - **EDVR's share there is about 3 ms of a 12 ms cycle**, of which draw hooks are 1.7-2.5 ms. That
    is a target for the render-thread CPU arc, not for this branch.
  - **Untested:** a cold-driver compile of the new shaders in VR. The flat first launch showed a
    possible +10-35 ms on one or two frames, once.
- **Still to check on a later flight:**
  - **Build identity:** `edvr_log.py --expect-build HEAD`.
  - **CPU-06:** the 30 s "ui quality: depth view memo" line, in VR and in flat. Expect hits to
    dominate.
  - **G09 counts:** the ZS/ZUI presence totals line, which gives how many dispatches skip each load.
  - **G09 timing:** the GPU census door "motion prep", against 0.374 ms a frame in the 2026-10-14
    hangar on the 5090.
  - **CPU-02 and CPU-04:** `tools\cpu_profile.py` samples under `uiLayerDecide` (CRT vsnprintf and
    malloc) and under `srv0IsPanelSizedSlow` by parent.
  - **Regressions:** the UI layer, holograms, ship trails and DLSS output must look unchanged. A
    regression here means a premise below is wrong; it does not call for tuning.
- **Open (not built):** see "Measure first" below. Each item has its own gate.
- **Issue 131 (2026-10-24, RocketsProjects):** a closed form for `engineStaleStampKind`'s 64-round
  search. Proven exact (each round is a bijection). Built on main, no key. The WARP rig holds it to
  the loop on 4,704 cases with 0 mismatches. Not flown. See "Issue 131" below.

## What was built

| ID | Change | Why it is exact | Proof |
|---|---|---|---|
| CPU-02 | `uiLayerDecide` builds the family-line detail text (`describeBlend`, `describeDs`, `_snprintf_s`, a temporary `std::string`) only when `noteFamily` will print it (`UiFamilySeenTable::wanted`). About 70-110 redirected draws per frame in VR used to pay for this text. | Nothing between the guard and `noteFamily` writes the seen table or rebinds a shader. First-occurrence text and decisions are unchanged. | ui_quality_test: the table checked against a frozen copy of the old rule; source scans on the guard |
| CPU-04 | The after-UI site asks `srv0IsPanelSized` only behind `!afterExcluded && uiLayerWorldScreenHeld()`. | `uiLayerAfterWritePreserved` reads `panelSized` only in that case, and `g_screenHeld` is written only at the frame boundary. | source scan. The probe's first-size note no longer fires for non-eye targets from this site; accepted |
| CPU-05 | `uiLayerNoteOther`'s four-slot PS SRV scan runs after the preserved-exclusion return. | An excluded write never reads the scan's outputs. | source scan |
| CPU-06 | `seedWritersCheck` remembers a successful depth-view resolve against (view, `bindingGeneration(Dsv0)`). The resolve is up to 4 guarded COM round trips a call, on every owner draw after the first take. | The generation moves on every OM rebind, ClearState, command list and frame boundary (the engine_velocity and temporal_pass idiom). Failures are not remembered, so the fault budget is unchanged. | a behavioural rig (ui_layer_world_test `testDsvMemo`, against the real `resolveBoundDsv`), plus hit/miss counts on their own line |
| CPU-12 | Setter-class `flatIssueEpochBump*` skipped while `runtimeVrProfile()` (flat_issue_epoch.h). | Only the flat Issue certificate reads the epoch. It is stamped only in flat draw scopes and reused only at the same epoch and frame. Leaving Flat clears it and revokes the epoch before `runtimeProfileSet` (mode_watch.cpp `commitPosted`), all on the render thread. Revoke and NoteResource stay ungated. | flat_temporal_test `flat_issue_epoch_gate_tests.h`: per profile, plus a Flat→VR→Flat walk |
| CPU-17 | The five Map-bounce note calls in vscreen.cpp are skipped while `runtimeVrProfile()`. That is about 3 locked increments × 1,100-3,000 Maps per frame. | `reportMapBounce` (flat Present only) is the only reader. Leases, install and `bankSamples` are untouched. | source scans |
| G09 | See below. | | |

**Side effect accepted for CPU-12 and CPU-17:** the setter-class and Map-bounce counters now count
non-VR time only. Before this, the first flat window after a VR stretch carried VR counts.

In the issue-epoch report's first window after a switch, the ungated `revokes` and `cbWrites` still
include VR time while the four setter counters do not. Ratios between them in that one window are
skewed; later windows are not affected.

## G09: optional smoke and UI-depth loads in `zSceneAt`

`zSceneAt` merged `max(max(Z, ZS), ZUI)` with three Loads on every call:
- **mv** (the trained path): 2.25 calls a pixel in the tile fill.
- **main** (native TAA): 9 calls a pixel in `fetchHistoryT`'s 3×3.

ZS (smoke depth, t6) is bound only while a ship trail drew this frame. ZUI (private UI depth, t7) is
bound only once some UI coverage prepared it. A null SRV still costs a fetch on NVIDIA: the UI
resolve's own null taps were measured at about 0.04 ms an eye per null input on the 5090 (the comment
in ui_resolve.h). This was P2 in edvr-gpu-trims-2026-10-14.md, "proposed, not built".

The new design uses two probe.w bits with *absent* polarity: 64 means t6 is unbound, 256 means t7 is
unbound.
- Each bit is computed from the very SRV placed in the bound array, so a bit can never disagree with
  the binding.
- A skipped load is replaced by the literal +0.0 that a null view returns.
- The `max` chain and its order are kept. A plain skip would not be exact: `max(NaN, 0)` is 0 today,
  and negative or -0 depth would also behave differently.

**RocketsProjects' independent bench** (2026-10-23, an RTX 5060, 2016² for one eye, 48-dispatch block
averages, 12 interleaved paired rounds × 3 runs):
- They used a compiled variant that drops only the ZS load: `max(max(Z, 0), ZUI)`.
- With private UI depth off, it took the mv fast pass from 0.310 to 0.245 ms an eye (−0.065, 21%).
- With UI depth on, it went from 0.258 to 0.230 ms an eye (−0.028, 11%).
- 72 of 72 rounds improved. The output was byte-exact on null smoke, with a bound-smoke negative
  control.
- This is a per-dispatch microbench. It is not a frame-rate claim, and it says nothing about how
  often smoke is absent in live VR. The new presence counter answers that.

**Built** (ab78bbf4, 42bf0a99, review fixes fa1afa36).

The shader:
- The Z load is unconditional. The ZS and ZUI loads sit behind `[branch]`; the DXBC puts every t6/t7
  load inside an `if_z`.
- `depthLayersAbsentBits` (temporal_pass.h) reads `srvs[6]` and `srvs[7]` from the array bound at the
  mv and main dispatches.
- The "motion prep depth layers" line counts mv dispatches by presence combination.

The native-TAA pass (main) reads its 3×3 scene depth from a 10×10 groupshared tile with a one-texel
apron (`[numthreads(8,8,1)]`).
- The fill runs only where the 3×3 does: history, depth, and a depth or world term on.
- The barrier is unconditional.

Proofs:
- **ui_depth_test zscene gate:** 1000 bit-pattern triples × 4 presence combinations, asuint-equal on
  WARP and the 5090.
- **The CPU half:** a behavioural test over 26 slot cases, plus source scans that the bits come from
  the very array bound before each Dispatch and precede `setParams`. Both were checked with mutants.
- **screen_consumer_test:** the five shipping blobs are byte-identical with bits clear and set,
  including -0, NaN, denormal and negative depths.
- **main_depth_tile_test:** O/N/UN/Stats byte for byte against the untiled main over 78 configurations
  with partial groups (split.x 0 and the diagnostic variant's candidate masks among them). A poisoned tile
  catches an under-filling fill gate, and a probe that counts the groups which wrote the tile catches an
  over-filling one. A mutant tile index is caught in all 22 configurations that run the 3×3.

Bench (RTX 5090, 2016² for one eye, RocketsProjects' method, outputs byte-checked first), in ms an eye:

| Pass | Case | Before | After |
|---|---|---|---|
| mv | UI depth off (both layers absent) | 0.0426 | 0.0370 (−13%) |
| mv | UI depth on (smoke absent) | 0.0433 | 0.0408 (−6%) |
| mv | all bound, bits clear | 0.0433 | about 1% faster (no regression) |
| main (native TAA) | depth on | 0.146 | 0.128 (−13%) |
| main | history reset | 0.059 (tile, ungated fill) | 0.034-0.038 |
| main | no depth | 0.128 (tile, ungated fill) | 0.088-0.094 |

- In mv, the gate matches RocketsProjects' compiled no-smoke variant and also skips the ZUI fetch.
- The 5090 times are about 1/7 of the 5060's, so mid-range cards save several times more in absolute
  terms.
- How often the layers are absent in live VR is still unmeasured. The presence line answers that on
  the first flight.

## Measure first (real, not built)

- **G08, in-place UI resolve** (P1 in gpu-trims).
  - `applyUiResolve` could write `dlOut` in place and skip the store where a pixel is unchanged.
  - Saving: about 0.013-0.027 ms an eye on a 5090, and about 0.05-0.09 on a 512 GB/s card.
  - Blocker: handing out the typed UNORM `dlOut` breaks the D1 typeless rule (temporal_pass.cpp, the
    `dlSubmit` creation comment). The alternative, a typeless `dlOut`, needs NGX, FSR3 and XeSS each
    to accept a typeless output.
  - Gate: an in-place variant benched in `ui_holo_pass_test --bench` on the 5090 and an AMD card
    (≥0.03 ms an eye), plus a byte-compare proving D1 no longer applies on the native runtime.
- **CPU-01, the profiler bracket.**
  - The X hook's `DrawClock` interval includes `weaponMotionDraw` and the census scope, so the "draw
    hook CPU" line under-reports EDVR's cost on foot.
  - Fix: take r0 and `realCall` around `realDrawIndexedInstanced` only.
  - It is a measurement fix, not a speedup. It was held out of this batch so the line stays
    comparable across it.
- **G03, bit-aware stencil freshness.** Sound for a stencil-only writer whose writeMask is disjoint
  from seededMask, but the measured hangar is already 2 seeds a frame (mask 00). Record
  `l->seededMask` in `g_seedCensus.event` and fly the late target-sprite storm
  (dlss-performance-review-2026-09-28.md) with diagnostics on; close if the residual reseeds are not
  disjoint-writer cases.
- **`dsReproducible` per-frame memo.**
  - About 8 COM calls on each of 62-93 tested takes per frame, all a function of the bound DSV.
  - The memo must handle a device reset.
  - Gate: a cpu_profile.py parent split first.
- **XeSS bridge copies** (xess_engine.cpp: output copy-back and three input copies per eye). The
  existing `kBridgeCopyIn` / `kBridgeCopyOut` timers in an XeSS log price them.
- **G13, near-light occupancy.** There is no exact change: an all-zero fallback map would read as
  "empty". If the census door "hologram resolve draw" shows ≥0.05 ms an eye, bound the draw and
  dispatch to the elements' screen extent instead.

## Issue 131: closed-form stale-stamp age (2026-10-24)

RocketsProjects asked to replace the 64-round loop in `engineStaleStampKind` with an arithmetic
inverse that recovers the stamp age directly. They reported deriving and validating one; the issue
carries no code, so the check below is EDVR's own.

**Where the loop runs.** One HLSL definition, in the shared engine-motion core
(temporal_shader_source.h, `engineStaleStampKind`). It is reached from the DLSS/FSR motion compose
(`enginePixelZ`), the on-foot screen source (fixed_shader_source.h, `sourceEngine`) and the flat mono
prep (flat_mono_shader_source.h). Each calls it only for a pixel whose fresh check read kind 3. Two
mirrors carry the same loop: emit_tests.h `staleKindOf` and tools\flat_pixels_engine.py
`stale_stamp_kind`.

**Why a closed form is exact.** A round is `f(s) = xs13((h ^ s) * 0x01000193)`, where `xs13(x) = x ^
(x >> 13)`. Each step is a bijection on 32-bit values:
- the XOR with `h`;
- the multiply by an odd constant mod 2^32 (inverse `0x359C449B`);
- `xs13` (inverse `y ^ (y >> 13) ^ (y >> 26)`).

So for each tag exactly one stamp `s` matches the marker, and `age = token - s` (mod 2^32) is the
only candidate. The loop's answer is the tag whose age lies in 1..64, or the smaller age if both do.
The two ages cannot be equal: one `s` gives one hash, and the tags differ. Two inverse rounds plus
the comparison give the same kind on every input, valid or garbage. The `token - age` wrap is the
same mod-2^32 arithmetic in both.

**Checked** (a scratch script, not committed): the loop and the closed form agreed on 611,600 cases.
- 49 tokens, among them 0, 1, 63, 64, 65, 0x80000000 and 0xFFFFFFFF, each with random seeds.
- Garbage markers.
- Real markers of both tags at ages 0, 1, 2, 31, 63, 64, 65, 66, 1000 and 2^32-1, and 200,000
  masked markers at random ages in the window.

The `xs13` inverse was checked separately, on all 16-bit values and 200,000 random ones.

Not covered by a case: a marker that matches both tags inside the window at once. A random marker
does that with probability about 2^-52, too rare to construct. The smaller-age rule is checked by
reading it.

**What it saves.** A kind-3 pixel ("pool surface not a rig record") pays all 64 rounds, because
garbage never matches; a stale-stamp pixel pays its age. The populations measured so far:
- 67,923 an eye-frame at a settlement pad (2026-09-23, Pimax, 1597x1835 render, 2.3% of pixels):
  about 4.3 M rounds an eye-frame, against about 136 k with the closed form;
- 2,616 at a station (build e1bf2dbd);
- stale stamp 0 in the 2026-09-26 coriolis flight.

At those counts the loop costs microseconds a frame on the 5090 if the pixels sit in whole waves.
The cost grows with how much of the screen is a non-rig pool surface, with how scattered those
pixels are across waves (one such pixel holds its whole wave in the loop), and on GPUs with a slow
32-bit integer multiply. No measurement prices it yet.

**Built (2026-10-24, no key: it is exact).**
- The core's `engineStaleStampKind` now calls `engineStampAge` once for each tag. That function undoes
  one round and returns the age. Only an age of 1..64 counts as inside the window, and the smaller
  age wins. The false-match note now reads ~128 * 2^-32, for two tags.
- `emit_tests::staleKindOf` keeps the loop as the oracle. math_tests `staleStampClosedForm` runs the
  shipped HLSL on WARP over 4,704 cases:
  - 14 tokens, four of which wrap the window through 0;
  - both tags at the ages above;
  - eight kinds of garbage each.
  The result was kinds 6/2/3 = 840/840/3024, with 0 mismatches against the loop.
  - The test also checks the edge ages by name: 1 and 64 read 6 or 2; 0 and 65 read 3.
  - Mutating either window bound in the shader text gave 168 mismatches and a failing rig.
- The on-foot "screen motion" ps_5_0 source pin moved to 0x2E25008DC3CE207F. That shader calls the
  function, from `sourceEngine`'s kind-3 branch.
- flat_pixels_engine.py keeps its loop. It runs offline, and both forms are exact.
- **Flight check:** the census pixel line's "not a rig record" and "stale stamp" counts, in a scene
  already measured. They should be unchanged in kind. Only a same-scene A/B of the motion pass, or
  a rig bench, would show the price.

## Ruled out (with reasons)

- ruled out: CPU-03 (skip DS/blend inspection on a known MRT refusal), because no MRT/UAV refusal
  appears in four current VR logs. The seeder is already warmed at the frame boundary, so the
  "first-use" side effect is moot.
- ruled out: CPU-09 (memo of blend descriptors), because it saves at most tens of µs a frame. A
  pointer key risks ABA, and a held reference or private-data lookup costs about what GetDesc does.
- ruled out: CPU-10 (FSS chrome metadata), because it is already behind the kind, count and VS-hash
  gates. That is about 6 draws a frame.
- ruled out: CPU-13 as a separate item, because it is the proof CPU-12 needed and saves nothing
  itself. The proof is above.
- ruled out: CPU-15 (negative cache for zero shader hashes), because the create hooks register before
  the game sees the pointer, so a healthy run has about zero such lookups. A negative cache would need
  a publication protocol.
- ruled out: CPU-19 (atlas watch after the instruments are disabled), because gating would break the
  since-created and 30 s totals. The saving is about 5-10 µs a frame.
- ruled out: CPU-20 (duplicate RTV query on a flat clear), because the two observers have different
  fault and type semantics, deliberately (design-flat-temporal-aa-2026-09-23.md). About 2-4 µs a
  frame.
- ruled out: G17 (fuse UI resolve with RCAS), because sharpening is off by default, it crosses two
  modules, and quantization exactness is per adapter.
- ruled out: G01 (OpenXR transfer), because it is already measured and closed in
  openxr-performance-review-2026-09-14.md, and frame_end_overlap defaults on.
- The 19 findings the audit itself marked NO-GO (CPU-07/08/11/14/16/18/21, G02/04/05/06/07/10/11/12/
  14/15/16/18) were not reopened. G01, G19, G20 and G21 are measurement prerequisites, not changes.
