# DLSS performance review — 2026-09-28

## Status

- **State, 2026-10-06:** proactive review of features added since `v0.18.2`,
  rather than a newly reported regression. Frozen source range `9a875295` to
  `12a381a6` (57 commits); branch `codex/dlss-performance-since-0182`.
- **Conclusion:** core VR eye DLSS, NGX evaluation, submission, and crisp HUD
  are unchanged. The shared on-foot world shader has higher compiled resource
  usage; weapon-history extraction adds CPU bookkeeping on admitted on-foot
  draws and raises its record limit from 64 to 128. These are candidates, not a
  measured frametime regression.
- **Open hypotheses:** actual GPU cost of the larger shared shader, admitted
  weapon-draw CPU cost, and whether VR exceeds 64 live records. Compiled
  baseline/current comparison and discriminators are in the dated entry below.
- **Evidence limit:** latest located Steam VR flight verifies its own source
  `373198c1`, but is earlier than the baseline and outside the reviewed
  lineage. No paired current/baseline capture or usable current Frontier log
  was found. Historical GPU/CPU timings cannot price this source delta.
- **Environment:** historical Steam flight used Pimax OpenXR/Crystal Super, RTX
  5090, 90 Hz, DLSS Performance/K, runtime 310.9.1.0, 2016x1949 input to
  4032x3898 output per eye. This is evidence provenance, not a verification of
  the current install. Any optimization flight uses Frontier and the user's
  requested SteamVR/OpenXR runtime.
- **Ruled-out pointer:** the dated review below separates flat-only features,
  own TAA changes, and unchanged VR routes. September's measured handoff and
  Present waits, flown fixes, exclusions, and selector unwind remain in their
  dated journal entries; recovery/diagnostic review B1/B2/B3 was not
  re-audited.
- **Next:** review complete; no flight or speculative install required. Measure
  the candidates before choosing a fix. Keep this branch separate from main.
  September Frontier checkpoint `967f0519` and trace `035907-fda516` are
  historical, not current-build evidence.

## Pre-optimization Frontier evidence

Historical rc3-7/`538175a8`, graphics `6ABA6DF5`, 12:36:05 census: EDVR ~4.949
ms/stereo, DLSS 3.129, UI depth .520. Reviewed paths unchanged through
`1d2e60ef`. Overlapping round-robin scopes are not a synchronized budget.

### Red bars and actual submit work

The graph's `Submit wall` label is misleading: ABI-v5
`NativePerfHistory::cpuFigure` publishes preceding `callerWorkMs`, from pose
wait return to next pose wait entry. It includes rendering, both submits and
post-submit work; excludes the next pose wait. 0.17.0/current benchmark CPU
rows use pre-submit `applicationMs`. At 90 Hz the graph turns orange above 1.02
periods (~11.33 ms) and red above two (~22.22 ms).

Older owner Submit p50/p95/p99 .423/.592/.738 ms excludes caller roundtrips;
12,625 overlaps/zero sync failures. Rare stalls remain separate.

## Source review against 0.17.0

DLSS retains copy→MV/depth/masks→NGX→UI; transfer separate. Added
engine/hologram/diagnostics cost work, retired separation removes work.
Resolve/NGX timing predates 0.17.0; setup gated/DONOTFLUSH polling. Old luma
blocking was possible, not proven; desk probes synchronous.

## Optimizations delivered in 0229c358

`91cf805b` gates/caps hologram diagnostics; `a001f669` exact16-row luma cuts
logical traffic99.55% with nonblocking retries/cached UAVs, no measured GPU
gain. `0229c358` combines crisp `472ff122` (eight families). Sphere/corona and
then-unproven reticle stayed in-scene. `ui_depth.cpp` remains necessary;
`fix.ui_depth` already retired; fallback needs stencil clear.

## 2026-09-28: combined Frontier flight and next changes

`141820`/`6ABACAED`, runtime `141822_298_35576`, verify `0229c358`: 12,937
pairs/zero sync failures, HUD accepted/GPU~9.5 ms. Seed .612/.841, machinery
.962/1.197 ms/eye, four seeds/three stale; OFF/ON scopes differ. UI125 has
1.621× UI100 pixels. HDR seed lies outside the primary-draw timer; reissue,
tonemap, coverage and seed prices overlap. `0a31d6ba` adds gated seed census
and GPU subprices, not freshness changes. Empty-clear bypass removes 4,096 heap
requests/256 clears, preserving unwind. Full gates/promotion/install pass.

## 2026-09-28: verified probe flight and guarded seed optimization

`151543`/`6ABAD7B5`, runtime `151544_659_18972`, verify `0a31d6ba`. All 36
invalidators/12 timelines are forwarded stencil-only writes, PS
AFED1D4B087E18A9/66B08F89E4926C01/A4D03619D631B186. Three stale seeds/eye;
3,360 GPU pairs price copy .028/execution .123/CPU .0047 ms/seed. Parent seed
.614–.623, machinery .936–.963 ms/eye. Guarded preservation retains legacy
private-write/raw-replay/read-only-view/late-unwind behavior across shared
sources and both eyes. WARP/RTX 2,896 each: four→one seed with exact
colour/depth/claimed stencil. `8d60b453` full gates/promotion/install/push pass
(`build/dlss-depth-seed-preservation-full.log`). Clear hit rate is not saved
CPU price; the short OFF period cannot establish cadence.

## 2026-09-28: guarded Frontier flight and original Steam baseline

`155419`/`6ABAE133` verifies `8d60b453`; user says better. Current SteamVR
dimensions/preset match Status. Exclude 15:57:15.962–20 transitions. OFF2,684
and ON2,698 frames: one seed/eye, zero stale/failures. Seed median/p95
.156/.189 versus .156/.166; machinery .485/.810 versus .485/.802 ms/eye. Full
benchmark OFF19/ON22, 2,700 valid each: GPU p50/p95/p99 8.841/9.582/9.912
versus 8.914/9.628/9.982 ms; CPU3.117/3.775/4.169 versus 3.192/3.890/4.245.
Scene varies; no causal diagnostics price. Three OFF reds, ON zero; owner
Submit .319/.380/.518, post57.013/88.890/26.227. Original Steam baseline is in
Status; exclude its 16:03:48–16:04:00 mode flips.

## 2026-09-28: slower-frame storms, cause unresolved

Verified `8d60b453` Frontier gfx `160518`/runtime `160519_481_49900`,
SteamVR/OpenXR 22:05:19–22:09:21 UTC. User: lower average, initial >11.1 ms
yellow storms/green gaps, later calmer; ships unconfirmed. Diagnostics OFF
16:06:30; GPU p50/p95/p99 benchmark 21 ends 16:07:39: 9.597/12.395/13.697 ms
(220 valid/two invalid); 22 ends 16:08:07: 9.252/10.944/12.549 (2,203
valid/zero invalid). Short scope changes/repeated hash; no full 30 s window.

Early stale 0 excludes reseeding as whole cause. Late target sprites coincide
with stale 1,050→5,330/seeds 7,486→15,990, guard 0; diagnostics OFF leaves
invalidator keys unknown. After 16:09:12 multiply 98.2 MB×2/loading transition,
memory 1366.7 MB.

Map wall max9.398 ms; factory burst47 textures/859.6 MB. Neither proves game
origin or GPU cost. xrEndFrame median/p95/max2.390/7.945/14.756 ms, handoff
2.685/7.512 includes deferred-finish queue. 20,963 overlaps/zero failures. 66
caller reds mostly post, largest548.591 ms. Discriminators: caller stacks for
resource waits, GPU contexts for preemption, finish/handoff for runtime
queueing. Prior capture lacked DxgKrnl; correlations are not GPU busy time.

## SDK deployment correction

`45da6ae3` Frontier `164641`: "this build has no DLSS SDK"; exclude fallback.
Auto-detection omitted NGX/FFX. Explicit `EDVR_NGX_SDK`310.9.1 and
`EDVR_FFX_DX11`3.1.2 passed full/promotion gates
(`build/cpu-gpu-capture-sdk-{full,promotion}.log`). Frontier `be70af2a`
verified, INI/DLSS unchanged; gfx170036 initialized NGX. Smokes had60 CPU
frames/zero loss, provider coverage only. Corrected flight follows.

## 2026-09-28: completed trace and visual dump

`174242`/runtime `174244_204_43604` verify `be70af2a`; dump `174605` has 16
paired DLSS-success/history frames, no resets. F9 started while loading:
23:43:55–23:44:30 has 20 caller reds, steady 23:44:30–23:46:05 has three among
8,494 cycles. ETW cycle N matches next-wait log sequence N+1.

Cycle5600 waits449.983/451.389 ms in game SleepConditionVariableSRW. Steady8385
includes165.496 ms CreateTexture2D wait through D3D11/NVIDIA/VidMM in222.029 ms
post-Present. Nearby~1.03GiB allocations/9–10GiB usage do not prove budget
exhaustion. Steady11170 compiler samples corroborate46 ms nightvision compile.
Other first-use compiles reach157 ms. Checkpoint embeds 76 finite variants with
exact source/flags/macros/profile/stage checks; arbitrary-float macros,
game-DXBC transforms and driver creation remain runtime.

Game3D submit→completion .835/5.797/7.023/15.431 ms p50/p95/p99/max includes
queue delay; no game3D submissions during allocation/compiler waits. System
copy preemption is not attributed to game3D. Ready tails do not establish
pervasive scheduler starvation. Individual query samples were missing. New ETW
uses explicit producer IDs, not coincident XR/D3D query counters;
Analyzer531/wrapper204 checks and60-sample transport smoke pass. V1
unavailable.

Saved VS71DD/PS2D03 has no screen reads, depthOFF, scaled-additive blend,
stencil81/ref1/EQUAL/KEEP. Exact-pair admission passes4,152 WARP/RTX each;
unknown PS stays stock. This is world brackets, not scanner <>. P/T/L0 precede
final crisp composition in `native_sharpen.cpp`. Exhaust blur persists DLSSoff;
user cancelled investigation.

## Exclusions and next flight

- ruled out: persistent submit blocking as main cause: owner p99<.74ms; long
  caller rows mainly outside submits. Rare stalls remain.
- ruled out: settings causing `45da6ae3` DLSS failure: SDK absent; exclude
  `164641` fallback.
- ruled out: diagnostics ON required for reds: steady OFF3/ON0; no causality.
- ruled out: lost deferred completions: >12,000/flight, zero sync failures;
  this does not rule out caller handoff serializing behind those completions.
- ruled out: DLSS-only exhaust blur: user sees it off; capture cancelled.
- ruled out: removing seed clear: Frontier `0xC000` needs per-bit fallback;
  WARP/RTX lack specified-stencil-ref support.
- ruled out: accepted HDR reissues causing stale counts: raw draws bypass owner
  hook; KEEP/read-only/skipped draws already excluded.
- ruled out: null-token merge bypass: failed unwind must revoke late claims.
- ruled out: bare depth/stencil seed bypass:631-check WARP/RTX later pixels
  differ after private writes.
- ruled out: original-effect-only guard: read-only game DSV becomes writable
  private DSV and alters later pixels.
- ruled out: keeping stale depth: later WARP/RTX pixels differ after private
  writes; guard alone does not invalidate cache.
- ruled out: read-only private DSV saving E508 seeds:194946/16488/402,484
  original flags0/depth disabled; matching flags changes no depth writes.
- ruled out: engine fetch causing3–8ms prep: slow windows lacked engine views;
  removed sphere coverage caused it. See September23 in [engine
  motion](kinematic-motion-injection-2026-09-19.md).
- ruled out: terrain residual: shadow removed~306 copies/frame; terrain-off
  inert. Read its arc before reopening.
- unresolved: regression versus0.17.0: no matched flight pair; graph boundary
  change does not prove unchanged performance.

Correlate exact sequence/Present/stacks before assigning periodic polling,
keyed-mutex, allocation or worker waits. Earlier externally disabled ETW is not
evidence of fast execution.

## 2026-09-28: validated checkpoint

`1ff8c224` full/promotion gates pass in
`build/dlss-crisp-storm-{full,promotion}.log`; NGX/FFX and optimized PDBs
verify. Installed/pushed separately. Graph is `Application wall`; no storm
resolution.

## 2026-09-28: recovered latest flight and corrected selector identity

Frontier `1ff8c224` gfx194044/runtime194045_548_19952 verifies. Interrupted
capture `194016-f80584` was recovered: 15,382 covered cycles, zero loss, 15,365
valid GPU joins/17 unavailable, matching PDBs. Original failed status is
preserved beside `recovery-status.json` and `report-recovered.json`. Save now
defers Ctrl+C for the same stop child within180s (220 checks); window closure
remains unprotected (review B8).

Quiet01:43:01–31 UTC versus target01:44:20–49 query p50/p95/p99:
9.129/10.648/11.719→11.136/13.508/14.466ms. Rise begins01:43:55 before first
world-reticle redirect01:44:17.669. No strict alternating cadence. Pre-target
query tails24.77–27.43ms include game-worker waits14–17ms; other tails do not.
Loading from01:44:50.169 is excluded. Mixed-target seeds 1→~3.73/eye and seed
median.154→.754ms also coincide with more sprites; no exclusive reticle
attribution or controlled benchmark. DiagnosticsOFF. Nightvision creation.259ms
replaces prior46ms compile; 28 shader creates .129–.655ms remain without
corresponding HLSL compile hitches.

Second verified gfx194646, dump194946, is separate from this ETW flight. User
pictures specify yellow <> on the scanner rim; the admitted71DD/2D03
world-space brackets are a different element. ruled out: this exact pair as the
desired scanner selector, because the user pictures establish the distinction.
Correction: `captureEyeRun(result)` runs before `uiLayerComposite` in the
native path; P/T/L0 cannot qualify final crisp pixels. Add a matched final
capture rather than infer success/refusal from those images. Actual hologram
admission was explicitly requested. Full build and fixture results are recorded
in the HUD arc (`build/dlss-holo-final-full.log`); see its final entry for
semantic evidence, guards and capture limits. No whole-frame saving claimed.
Existing probes miss scanner centers(.5861,.5916)/(.5939,.5919) and cannot
identify private HDR; shared94D5 remains unadmitted.

Tight red-hull ROI in16487/88/89/95/502 has82–86% joined engine motion. ruled
out: wholesale missing hull motion here; joins do not prove accurate vectors or
explain blur. No sharpening.

## 2026-09-29: holograms confirmed; ship-approach CPU stalls unresolved

Frontier gfx `033435`/runtime `033436_691_11608` verify installed `7aaaf39c`
(`6ABB2662`, PID11608), SteamVR/OpenXR at the Status dimensions. User confirms
good holograms and fpsVR CPU yellow spikes >11.1 ms, worse approaching ships.
Final dump `033645` writes 34/34, scenes 12224–12239/native 10555–10570 match;
32 composition true, first right crop/overview passthrough. Both models are
absent P/T/L0 and visible Final. Scanner contacts overlap the yellow <>;
existing probes miss it. Final changes world brackets and 30–34% bright pixels
in a compact ship ROI with sharpening OFF: overlay contribution, not proof of
ship mesh admission. Exclude 03:36:45.300–49.772 capture/readback.

ruled out: new remap causing the earlier storm, because its first admission is
03:36:00.207, after 140/142/231 ms post-submit stalls at 03:35:57–58. Of 38
logged pre-dump long cycles, 35 are post-submit dominant. Pre-submit median
.645→4.118 ms and post-submit 1.232→5.533 across adjacent30 s windows. Raw
Present max 1.114 ms versus outside-Present 226.314 ms in the later window;
these are aggregates, not an exact-cycle attribution. Native benchmark omits
post-submit/submit waits, so 4–6 ms medians do not refute fpsVR spikes.

Resource bursts reach 907.4MB and owner Map6.396ms, origin unpriced. DLSS
median3.23–3.38/p954.26–4.48ms/stereo stays steady. HUD03:36:35: 58.78
redirects/29.12 stock writebacks/stereo,2.367seeds/eye; seed.493/1.536 and
machinery.971/2.023ms median/p95/eye. Four remap draws, zero refusals, two
shaders prepared once. Workload differs, diagnosticsOFF, 20 scope-changed
benchmark rows: no exclusive remap cost or controlled baseline.

The next completed trace uses existing instrumentation: caller running, ready
and blocked stacks+wakers; owner finish and GPU submission queues.
DiagnosticsOFF, no eye dump; query spans can include CPU submission gaps.

## 2026-09-29: completed profile proves handoff serialization

`035907-fda516` verifies installed `7aaaf39c`, PID 28296, gfx `035926`/runtime
`035927_475_28296`. Capture complete, 5,861 derived/covered cycles, zero loss,
5,845 valid GPU joins/16 unavailable, two matching PDBs. Use UTC 10:01:03.105
through 10:02:16.042; loading UI returns afterward. User reports mainly CPU
spikes, fpsVR GPU <=6.8 ms. Query median 12.761 ms includes submission gaps,
not GPU busy time. No approach timestamp established.

Hypothesis CONFIRMED before editing: PostPresentHandoff reblocks the producer
behind the deferred frame end. Steady 4,846 cycles show ~1.944 ms/frame at this
wait site; proxy self samples ~.826 ms/frame in full interior windows. Exact
sequences 9570/10000/10239 wait 2.103/1.919/1.935 ms: `_Cnd_wait ->
OwnerService::invoke -> NativeRuntimeHost::handoff ->
OpenVRCompositor::PostPresentHandoff -> game4e1abb`. On 10239 owner 51836 is
inside SteamVR xrEndFrame through finishPendingFrameEndBody; caller 50772 later
runs 3.435 ms after Present. Source handoff only validates/increments a
counter. A nonblocking notification can permit real game work to overlap; do
not claim a measured whole-frame saving before flying the change.

Separate 10240 hitch: 291.728 ms cycle, query 12.883 ms, 201.961 ms longest
wait through game CreateTexture2D/D3D11/NVIDIA/dxgkrnl/dxgmms2, woken by PID
4/TID 160. Game-owned queues receive no submissions for 290–301 ms. Actual
created RT 4862×2735 fmt27 follows that wait; a preceding RT/depth burst has
2917×1671/2674×1671. Gfx identifies 4862×2735 as scanner chrome; engine panel
sizing is ×2.5000, but this oversized surface fails the sizing-chain gate. Its
original stage and memory budget are not established. The FSS hook only doubles
exact half-eye sizes, excluding these odd physical dimensions. Seq 8908
producer-copy driver wait 14.153 ms wakes from NVIDIA worker 50000; next queue
submission follows .256 ms later. These are CPU underfeeding witnesses, not
proof of GPU busy time or paging exhaustion.

Artifacts: `build/capture_probe/handoff-witnesses.json`, retained ETLX, and
`build/gpu-approach/allocation-witness.jsonl`. Keep the handoff change scoped
to valid overlapped pairs; preserve synchronous paths, lifetime, generation,
frame completion and next-operation ordering. Test with blocked fake XR.

Implemented one-use admission for successful separate-device overlapped pairs.
Queue the value-only handoff behind finish and ahead of subsequent caller
operations; synchronous/borrowed/turbo paths retain validation. A mutex and
caller-operation epoch prevent stale publication. Actual-host tests hold XR end
open while real PostPresentHandoff and producer work return; also cover FIFO,
invalid generations, queue full, cancellation and close fallback. Focused
native: 4,877 checks/zero failures; independent review finds no must-fix
defect. This preserves existing serial frame admissions, without claiming
concurrent Wait/Submit support. New close summary distinguishes
accepted/completed/rejected/invalid/cancelled-or-failed jobs.

Full `build.bat` validation passes, including 86 pooled rigs, Python gates,
production DLLs, config contract and installer resources. Log:
`build/dlss-handoff-review-full.log`; input receipt
`aee14f4390edea1513b0186f175289688aa3f46e2b746871652fcbb288440702`.

Source commit `6eede364` is pushed separately. Clean receipt-guarded promotion
passes in `build/dlss-handoff-review-promotion.log`, version
`v0.18.0-rc.3-63-g6eede364`. Sanctioned installer dry-run/install/verify pass
for Frontier; full output in `build/dlss-handoff-frontier-install.log`. INI
SHA256 `A2D27168…784273DF` and installed DLSS `BE6E434A…FB6EE6E` remain
unchanged. Capture helper now expects this code; dry-run starts no trace or
workload and writes nothing. Post-fix flight is recorded below.

## 2026-09-29: external review and slices triage

Read both main-checkout review files (snapshot 7aaaf39c), including raw X1–X3.
B4 repeats a percentile sort; preserve exact interpolation while sorting once.
B9 own-query comparisons can be skipped only with no own handles. Preserve
pre-arm game-query tracking: an already-open query can outlive any arm grace
period. Case-fold accepted census values without changing flat-profile policy.
B4/B9 implemented; production-header UI rig passes 4,156 checks and both
graphics source files compile. Partial query creation conservatively keeps the
scan enabled until every handle is released.

B3 confirms detailed route timers run with diagnosticsOFF. Existing key says
off reports machinery; requested permission before gating it. Basic application
frametime is separate. Review estimates and capped samples do not establish
these costs as the cause of multi-millisecond storms.

B1/B2 remain before-main fault-recovery work: retained dirty PS/b13 must be
restored before unblocking draws; clearing the latch alone is unsafe. Shared
HDR/LDR budgets and retained HDR resources also need scoped recovery tests. No
fault signature in this flight establishes them as its timing cause. B5/B6
admission/production-test coverage and B8 capture window-close cleanup stay
open. Build-tool refactors, shader goldens and broad module rewrites are
deferred; main stays unchanged. The seed saving requires NVIDIA's per-bit
fallback; specified-stencil-ref devices do not take the same full seed.

## 2026-09-29: handoff flown; remaining CPU attribution

Gfx `045517` and runtime `045520_234_12728` verify installed `6eede364`, PID
12728, build 6ABB9884 and the Status environment. User reports mostly steady
90fps with a very distant station, CPU about 7–9ms. Diagnostics OFF; no eye
dump event. Shader dump was ARMED at startup, so shader-creation/file-write
boundaries remain a confound. Exclude loading return 10:59:31.792Z and shutdown
37.314Z. No new WPR capture exists; the station condition is user-reported.

ruled out: the old handoff wait explains remaining CPU, because full window 8
10:58:43.269–10:59:13.270Z measures .001/.001ms p50/p95 and final notifications
accepted 21558/completed 21558/rejected 0/invalid 0/cancelled 0. This confirms
the mechanism, without establishing a controlled whole-frame performance gain.
Window 8 has 2597 scene-ready cycles: game before first submit 6.001/7.148ms;
submit roundtrips .277/.487 + .203/.339; trailing callback 2.510/4.902; outside
Present 2.044/2.616; owner finish 2.824/5.177 and xrEndFrame 2.187/4.694. These
are wall p50/p95, nested scopes; do not sum quantiles or call them busy.

Source identifies a second possible serialization: after real Present,
NativeRenderBinding pumps one work item and runs frameWork. Its RenderRoute
synchronously invokes owner loadingBoundary behind deferred finish and handoff.
Steady loadingBoundary can return immediately, yet admission already waited.
Aggregate callback/end-frame similarity is consistent, not an exact witness.
Next CPU-only trace must join callback QPC to dispatcher wait stack and owner
finish/waker, versus executing treatment/NGX/driver, loadingStep/event work or
ready delay. Existing markers/stacks cover all these; no speculative fix.

Workload changes too: mover records 3→117–120/frame, blend binds 29.5k→105–107k
per roughly 5200 eye-frames; UI redirects 22.4→62–64/frame, writebacks 14.3→31.
The sparse draw-hook estimate .26→.92–.96ms can include private reissues and
driver waits, so is not exclusive CPU. Cached old 7aa steady trace now has
uncapped matching-PDB leaf aggregation: caller EDVR direct self .852ms/frame
(proxy R1 .6302); largest leaf beginPanelOverride .0530, readPool .0394,
hookedMap .0374, uiDepthOnEyeDraw .0322. Worker noteDirectBuild .0896 is
concurrent work, not caller delay. Direct self excludes induced driver work and
waits; old capture cannot attribute this new flight's total CPU. Artifacts:
`build/capture_probe/self-{samples,regions}.json`, 1ms sampling interval, zero
unresolved EDVR PDB symbols. There are 341 missing sample stacks across all
threads; sampled estimates are not exhaustive exact timings.

Use the existing admin capture helper with `-CaptureSeconds 60` and omit
`-GpuQueues`: start F9 after loading in the same scene showing 7–9ms. Keep
diagnostics OFF/no eye dump; let saving finish. It expects installed 6eede364.

## 2026-09-29: CPU trace confirms redundant Present rendezvous

`052039-ca6860` is complete, expected `6eede364`, PID 30932; both PDBs match
age 6, zero lost events, 5,257/5,257 valid derived/covered cycles, valid clocks
(.9 us max residual), no region unknown/residual. Capture 11:22:41.360Z through
save start 11:23:41.501Z; later loading at 11:23:52.771Z/save tail is excluded.
Full scene-ready window 5 ends 11:23:27.792Z: game-before-submit 6.415/7.841 ms
p50/p95, 2,618 cycles. No GPU provider; elapsed GPU markers are not busy time.

Hypothesis CONFIRMED before code edit. Seq 8490, UTC 11:22:54.197841Z: Present
begins at QPC 577878165638 us; caller parks at 165733.8→167736.1 us (2.0023 ms;
later QPC values omit the common prefix 577878). Stack `_Cnd_wait ->
RenderThreadDispatcher::invokeOwner:113 -> RenderRoute::invoke:43 -> loading
lambda -> NativeRenderBinding::callback:100 -> renderBoundaryPresent:189 ->
hookedPresent:1683`. Owner 49972 is sampled at 166774.6 us inside
NVIDIA/D3D11/vrclient -> SessionState::finish:305 (xrEndFrame) ->
finishPendingFrameEndBody:1341 -> OwnerService::run. It wakes the caller
through OwnerService::complete:144 at 167736.1 us; Present ends at 167762 us.
Steady 5,062-cycle site mean .6248 ms/frame, p50 .6049, p95 1.006, max 5.5634.
This resolves the aggregate ambiguity; loading callback reserializes finish.
Exact witness: `build/capture_probe/current/callback-witness.json`; spans and
uncapped samples are beside it. Steady seq 7454–12515 covers
11:22:42.006392–11:23:39.991580Z. Caller 28384 averages running 9.1158 ms,
ready .7308, blocked 1.6106; callback park is .6248, next pose wait .5161.
Direct EDVR self ~.8497 (R1 proxy .6586), game executable ~4.4577 sampled
ms/frame; induced driver work remains outside self totals. Separate seq9081 has
a 97.2233 ms graphics-memory texture-create wait, not this recurring cause.

Bounded design: this successful deferred pair already has finishSubmitTail's
sceneSubmitted (or fatal failure), leaving no loading work for its Present. Use
an independent one-use epoch-protected token, not owner LoadingState reads.
Invalidate on frame admission, loading-policy changes, failure/restart/stop.
Keep normal synchronous loading boundaries for every other case. General async
loading work is unsafe because graphics calls require an explicit borrowed
render boundary. Fixture must exercise actual NativeRenderBinding callback,
held xrEndFrame, producer progress, next-Wait ordering and loading fallback. No
whole-frame saving claimed before flying the validated change.

Implemented without new queued work or owner-only caller reads. The token is
published only with an accepted deferred finish and consumed independently of
handoff. Public loading mutations invalidate before dispatch; fatal failure,
restart, stop and close retire it. No-token Presents retain the existing
borrowed render boundary. The final focused native rig passes 4,904 checks,
zero failures (`build/capture_probe/present-bypass-focused.log`); actual
binding callback returns before held xrEndFrame, one-use/epoch invalidation and
next-Wait FIFO pass, and loading/failure/queue/shutdown cases retain their
contracts. Independent source/fixture review found no production blocker.

Full absolute-path validation passed (`build/dlss-present-overlap-full.log`):
86 pooled jobs in 116.5 s, native 4,904/0, UI quality 4,159/0, Python
self-tests, production DLLs, 264-key config contract and actual self-contained
installer resource checks. Receipt input hash
`45f4e404fdec842db22dc993da1dd4146e9de077309cc03aa2f242cd79b37791`; NGX/FFX and
optimized profiling-symbol build context unchanged. Commit this validated
source before receipt-guarded clean DLL promotion; Frontier only.

Clean source `c668f83f` pushed to `origin/codex/dlss-performance-review`;
receipt-guarded `--dll-only` promotion passed with `v0.18.0-rc.3-66-gc668f83f`
(`build/dlss-present-overlap-promotion.log`). Frontier dry-run, install,
payload verify and native-receipt verify passed; receipt
`edvr_native_receipt.json.pre-c668f83f-20260929-054723.bak` records the
matching pair. Both DLL version resources agree. INI and installed DLSS SHA256
remain unchanged, as recorded with full DLL hashes in
`build/dlss-present-overlap-frontier-install.log`. Steam and main unchanged.
Capture helper now expects this source; its CPU-only 60-second dry-run wrote
nothing, started no workload/session and left no output directory. Next is a
normal flight; any whole-frame gain remains unmeasured.

## 2026-09-29: Present bypass flown; selector unwind requested

Frontier gfx `055138` and runtime `055140_455_44976`, PID 44976, verify source
`c668f83f`/build `6ABBA511`. Sean reports "Much better." Runtime teardown
records 13,978 overlapped pairs, zero synchronous pairs/failures; handoff
accepted/completed 13,978 with zero rejected/invalid/cancelled-or-failed, and
Present bypassed 13,978. This proves the new route ran, with positive user
feedback; no controlled whole-frame gain or exact 0.17.0 regression
attribution.

The user explicitly requested unwinding only the <> selector experiment and
will have another agent address it. The attempt admitted world ship brackets
`71DD8B8B09060A81/2D037A047171BF3B`, not the still-unidentified scanner glyph.
Remove that crisp admission, extra PS observation and dedicated fixture;
preserve original world-marker depth/motion, working cockpit holograms, fixed
shader precompilation, capture infrastructure and all performance fixes. See
the HUD arc's final entry for exact scope and focused 2,904/0 evidence.
Independent review found no scope/regression issue. Full validation passed all
86 jobs in 125.9 s, UI quality 2,907/0, native 4,904/0 and installer gates
(`build/dlss-selector-unwind-full.log`); receipt input
`66d05e4444e0f1e7f767c6b6d6d2f69f10d3a4a96aee36ea27f55034ce2cd210`.

Clean source `967f0519` pushed; receipt-guarded DLL promotion passed as
`v0.18.0-rc.3-68-g967f0519` (`build/dlss-selector-unwind-promotion.log`).
Frontier dry-run/install/payload/native-receipt verification passed, with
receipt `edvr_native_receipt.json.pre-967f0519-20260929-061148.bak` and
unchanged INI/DLSS hashes (`build/dlss-selector-unwind-frontier-install.log`).
Capture helper now expects this source; CPU-only dry-run succeeded without
writes, workloads or trace sessions. Main and Steam unchanged; no additional
flight is requested for this removal.

## 2026-10-06: proactive review since 0.18.2

Sean requested a proactive review of new features, with no new slowdown
reported. Scope is tagged `v0.18.2`
(`9a8752958f394be6872015721fe336b43f115e38`, 2026-10-04) through frozen
`12a381a65f1dd44b0c4a62b5c4f18a63a5d64f9f` (2026-10-06), 57 commits. Other work
advanced `origin/main` during the review; it was not merged into
`codex/dlss-performance-since-0182`.

### Findings and reachability

**Measure first: higher static resource usage in the shared on-foot world
shader.** The VR world route's shared `prep` and HDR `finishHdr` shaders grew
even though VR leaves the new foreground-map branch disabled. Compiling both
source revisions with identical production entry points, profiles, macros, and
flags gives the following DXBC/disassembly comparison:

| Variant | Baseline bytes -> current | Reflected instructions | Virtual temporary registers | Bound resources |
| --- | --- | --- | --- | --- |
| `prep`, `cs_5_0` | 17,992 -> 19,780 | 560 -> 618 | 20 -> 21 | 15 -> 16 |
| `finishHdr`, `ps_5_0` | 5,648 -> 6,148 | 137 -> 147 | 6 -> 8 | 7 -> 8 |

Declared DXBC temporaries and static instruction counts are not measured RTX
register occupancy or executed instructions. A uniform false branch can avoid
new sampling while the driver still compiles a larger program. Thus a potential
VR GPU cost remains open, rather than being ruled out by the foreground gate
alone. Hardware timing of these passes with foreground disabled is the
discriminator; no shader specialization or rollback was shipped.

Reachability is narrower than the ordinary DLSS eye path. The separate
`experimental.temporal_aa_on_foot_world` route defaults to `auto`
(`vr_world_route.cpp:695-704`). Watching requires an active machine, live UI
layer, and held world screen (`vr_world_route.cpp:712-713,894`); treatment
requires the HDR/depth/camera selection and render size at least screen size
(`vr_world_route.cpp:277-306`, `flat_hdr_route.h:446-449`). Normal undersampled
DLSS eyes do not run these mono shaders. This is a conditional on-foot world
cost candidate, not an added cost to every cockpit DLSS frame. The flat-only
`experimental.temporal_aa_before_post` key is unrelated.

**Low priority: added CPU bookkeeping in admitted VR weapon draws.** Commit
`6ab6acff` extracted the existing animated-vertex history into
`AnimatedVertexHistory`. Its `prepareCapture` now calls `GetDevice`
(`animated_vertex_history.h:136`), and the VR consumer calls it again
(`weapon_motion.cpp:152`); the baseline made one such call. The owned capture
also adds six fixed COM AddRef/Release pairs, plus two per prior candidate (up
to four candidates), compared with the baseline's raw borrowed views:
`animated_vertex_history.h:129-130,167-173`. These are real additional
operations, but their CPU time has not been measured. The geometry metadata
queries and two record-matching scans already existed. No new steady-state heap
allocation was found below the record/byte limits after warmup.

This path requires weapon stability, a recognized on-foot weapon/tool VS, and
the matching on-foot source depth (`screen_motion.cpp:260-313`,
`weapon_motion.cpp:132-145`, `vscreen.cpp:4944-4949`). Ordinary cockpit eye
draws do not enter it. The extraction preserves the GPU index copy, one
identity dispatch, original-VS stream-output capture, and motion raster. It
does add a constant-time epoch/record check after capture; the fallback record
scan requires an intervening invalidation/erase.

**Low priority, conditional: more weapon history may now be processed.** Commit
`eccfce7a` raises the shared live-record ceiling from 64 to 128
(`animated_vertex_history.h:17-20`). The position-history byte cap remains 32
MiB, with small identity resources and metadata outside that accounting.
Records beyond the old limit can now allocate and execute identity/capture/
raster work instead of being refused, and matching scans can visit a larger
set. This permits intended motion coverage during equip bursts; reducing it
would trade correctness for cost. The observed `records=64` motivating the
change was a flat flight. No VR record-count evidence establishes how often the
larger limit is reached. Do not reduce the cap on this review alone.

**Direct VR eye DLSS and crisp HUD: unchanged.** `temporal_pass.cpp`,
`temporal_shader_source.h`, `dlaa.cpp`, `fsr3_engine.cpp`,
`native_temporal.cpp`, `vr_world_route.cpp`, `ui_layer.cpp`, and
`hud_layer_census.cpp` have no source delta in this range. No new eye-sized
allocation, readback, clear, copy, NGX evaluation, UI replay, or GPU query was
added to those routes. The NGX SDK pin/fetch and package-copy path are
unchanged; this does not verify a live `nvngx_dlss.dll`. Generated shader
bytecode is ignored and is rebuilt from source; the generator adds flat
foreground variants rather than changing the VR eye shader.

**Shared mono shader: changed, with new work disabled in VR DLSS.** The VR
world route does use `flat_mono_shader_source.h`. Its frame is
value-initialized and leaves the new foreground fields empty
(`vr_world_route.cpp:321-415`); `flat_mono_resolve.cpp:965` consequently leaves
the foreground control zero. The new foreground-map sampling path is not taken
there. The new four-texel history-depth test runs only in own TAA, not in DLSS.
Disabled branches alone do not prove identical register pressure or shader
execution time; the compiled comparison is recorded below.

**Flat observers and allocations: gated out of VR.** New replay-query calls at
`vscreen.cpp:4390,4405,4412` check `runtimeFlatProfile()`, a plain comparison
of the cached profile (`runtime_profile.h:80`). New shader metadata capture at
`device_hook.cpp:705-713,2719-2724` is flat-gated, and the shader cache returns
before work outside flat (`device_hook.cpp:629-640`). Engine velocity retains
the VR `R32G32_FLOAT` target; new marker format/resources and blend state
require flat provenance (`engine_velocity.cpp:761-776,1005-1030`). The extra
MRT6 condition is one profile check, with no added VR GPU pass.

Flat mixed-camera SDK AA has a full-render-size RGBA32F foreground map, cleared
and replay-drawn at H (`flat_foreground_motion.h:192-217`), gated by flat
runtime, SDK mode, and `selected.mixedCamera`
(`flat_runtime.cpp:4094-4095,5157,5438`). That can be expensive in flat, but
the state and calls are not reached by VR. Flat's GPU resolve timer begins
inside the resolver (`flat_mono_resolve.cpp:975`) after this preparation, so it
excludes the map work; the whole-frame GPU timer includes it. This is a
measurement limitation, not evidence of a VR regression.

**Timing, jitter, and submission: unchanged for VR.** The optional jitter count
in `temporal_math.h:118-121` defaults to eight, and the VR call at
`native_temporal.cpp:357` supplies no alternate count. OpenXR source,
`native_perf_history.cpp`, `perf_monitor.cpp`, and graph thresholds have no
delta. ABI-v5's monitor CPU figure still uses `callerWorkMs`, while native
benchmark CPU is pre-submit application time. A low benchmark CPU row does not
exclude post-submit caller stalls or explain a red monitor bar.

### Existing flight evidence

The sanctioned locator found Steam graphics `edvr_gfx_20261004_143345.log` and
runtime `edvr_openxr_20261004_143347_525_57808.log`. `edvr_log.py
--expect-build 373198c1` verifies their source
`373198c16538609638a8c1df622800e288a448e7`, version `v0.18.1-19-g373198c1`. The
explicit `v0.18.2` check fails; that source is not an ancestor of the frozen
review head. These are historical evidence only. The locator found no usable
Frontier log for the review target.

That Steam run used EDVR native OpenXR over Pimax OpenXR, Pimax Crystal Super
at 90 Hz, RTX 5090, DLSS runtime file version 310.9.1.0, Performance/preset K,
2016x1949 input to 4032x3898 output per eye. Driver version was not logged.
Completed benchmark window 9 had 2,011 valid samples over 30 seconds: CPU
p50/p95/p99 7.118/9.144/11.048 ms and GPU 12.082/13.758/15.598 ms.
Scene/scope/configuration and source are not matched to a baseline/current
pair. Neither these numbers nor the older 0.16.2 OpenVR comparison measures the
cost of features added since 0.18.2.

### Validation and next discriminators

This is a source/reachability review, not a measured whole-frame A/B result. No
production code, configuration, installed DLL, or DLSS runtime was changed.

The shader comparison used `D3DCompile`, `D3DDisassemble`, and `D3DReflect`
from `C:\Windows\System32\d3dcompiler_47.dll`, version 10.0.26100.9457, SHA256
`2E3526354DBCD9CF013F7B741C549DF2170FE6AF607628442216A21E7D584883`. That is the
production generator's resolved compiler path. Entry points, profiles, source
names, no macros, and compile flags 0/0 match the unchanged production
mono-variant table. It compares source-generated bytecode, not the currently
installed DLL or NVIDIA driver machine code. Scratch outputs are under ignored
`build/gpu_shader_compare`; the results above are retained here so the review
does not depend on those temporary artifacts.

For the weapon candidate, compare admitted `weaponMotionDraw` CPU intervals in
the existing offline WARP rig at one and 32 matching records, baseline and
current, after warmup. Include state restoration and final COM releases in the
measured interval. A WARP result isolates implementation overhead; it is not a
hardware DLSS frame budget. A separate 64 versus 65-128 distinct geometry-key
workload can confirm admission, allocation, scan, and GPU-work counts above the
old cap. Existing `--bench-history` is a reconstructed production-history
correctness/pressure test, not a frametime benchmark. VR weapon logs have no
live-record budget line: `records=` is flat-only. If real VR cap usage becomes
material, instrument it before a Frontier flight.

No fix is justified by an unmeasured cost alone. Any future flight must first
verify the exact build, and match SteamVR/OpenXR runtime, headset/per-eye size,
DLSS DLL/preset, scene, and diagnostics state. Keep the review branch separate
from main and use Frontier for optimization testing.
