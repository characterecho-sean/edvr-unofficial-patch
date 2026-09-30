# Native OpenXR performance review — September 14, 2026

## Status

*Added 2026-09-24. Restates the journal below; update it whenever this doc
changes.*

- **State:** on main and FLOWN 2026-09-24 (entries below): the long-cycle
  breakdown, p99/max, late frames, producer-copy GPU timing, thread
  priority, and Meta metrics work. `frame_end_overlap` first broke the
  Application-render GPU timing; with the timing retired before Submit
  returns it FLEW CLEAN (flight 124504, Pimax OpenXR: invalid 5, Elite's
  second-Submit park p50 0.16-0.18 ms) and now DEFAULTS ON.
- **Long cycles and the carrier, 2026-09-29 (entries below):** phase 0 ruled
  out the periodic jobs; flight 090608 ruled out EDVR's Present hook; 094331
  had 138-160 ms stalls with anti-aliasing off, so the stalls are Elite's.
  The instruments FLEW (112704): with DLSS at the carrier the GPU never
  waits between frames (gap p50 0.06-0.10 ms; 4.5 ms with AA off), so the
  frame is GPU-bound, and engine motion costs Elite's render thread
  0.35-0.40 ms p50 (ruled out as the limit). The GPU frame is 12.5-12.7 ms:
  Elite about 6.5, EDVR 6.0-6.2 (upscaler 3.08, UI and hologram passes 2.16,
  motion 0.46, and 0.3-0.5 inside Elite's own draws). On main since, not
  flown: the engine-motion clock sampled (about 0.12 ms a frame of its own,
  was 1.17) and the census naming the six "other fix-wrapped draws" fix by
  fix (entries below).
- **Comparing with 0.16.2 in fpsVR:** under OpenVR its CPU frame time is
  poses ready to second submit (0.16.2's `appCpuMs`); on the native runtime
  it tracks Elite's whole frame outside the wait. Compare pre-submit windows.
- **Open:** issue #38 (rc.1 at 90-99% GPU against 0.16.2's 60-62%) and a
  second report (jntracks: 0.6.0 in the 90s, 0.18 in the 80s with temporal
  AA off, same 2604x2644 per eye; the runtime path unmeasured). The census,
  its overcount fixed 2026-09-25, has since flown (the 09-29 entries). The
  overlap is unflown on the Quest runtimes; the depth layer is set aside
  (Sean, 2026-09-24); a controlled OpenVR comparison needs a v0.16.2 build.
- **Closed:** sections 5 and 6 below (the private and producer copies): the
  producer copy measured 0.039 ms p50 per eye at 4100x3962, under the
  0.1 ms bar.
- **Ruled out:** at the end of each 2026-09-24 entry, and in the 2026-09-29
  ones (the periodic jobs; EDVR's Present hook; EDVR's AA work as the
  stalls' cause; engine motion's CPU as the carrier's limit; the six wrapped
  draws as a large EDVR cost, being the pad rings the glare fix claims and
  the scanner-body resolve; for the UI and hologram passes, every cut but
  one, measured; the shared-pair rule as wired in 9122f31e, reverted).
- **UI and hologram passes, 2026-09-29 (entry below):** one exact cut, the
  UI resolve skipping inputs it was not given (about 0.09 ms a frame); a
  tile early-out, a groupshared window, R8 history, a composite tile bound,
  hologram scissors and one shared depth copy measured and ruled out.
  Largest left: the HDR HUD's depth seed (about 1 ms a frame at the carrier;
  a census section of its own since, built, not flown) and UI quality 1.25, a
  setting (1.62x the pixels of 1.0).
- **Next** (flight 132352 entry): the carrier sits at the budget (GPU
  10.7-11.3 ms, EDVR about 4.8 of it). Open: fly the HUD depth seed's census
  section (on main; then UI quality 100 against 125 in one flight) and stock
  glare's extra 2.5 ms. The landing-pad rule was merged, flown and REVERTED
  (flight 162819 entry): it unpinned the sun glare. Any Quest flight with
  the default build: check the Application-render GPU invalid count stays
  near zero and `native_frame_end_overlap_summary` reads failures=0.
- **Environment:** the numbers in the entry are Pimax Crystal Super, 90 Hz,
  separate device: Pimax OpenXR at 2600x2514, SteamVR OpenXR (`aapvr`) at
  4100x4050 and 2665x2087.

## The original review, 2026-09-14

The first implementation wave is tracked in the [submission optimization
notes](openxr-submit-performance-2026-09-14.md). The review below retains its
original source baseline.

The best immediate opportunities are reducing render-thread rendezvous and
simplifying the final XR draw. The largest architectural opportunity is
removing an intermediate full-resolution eye copy while retaining the separate
OpenXR device. Hidden-area masking is another promising omission: it could
reduce Elite's own pixel shading, whereas optimizing the final blit only
reduces EDVR's submission cost.

This is a static review of `b00edcb8c06be28e510adf8563df1aa17eac050b`. No
rendering code, installed DLL, runtime selection or live setting changed.
Opportunities below are confirmed code paths, with performance benefits still
to be measured. They are not an explanation proven for the earlier 9.5 ms
versus 11.1 ms comparison.

The newest Frontier OpenXR and graphics logs available during this review
identify `v0.16.2-97-g83dda1b-dirty`; `tools/edvr_log.py --expect-build HEAD
--version` rejected them against `v0.16.2-123-gb00edcb`. No current-build
flight baseline or new GPU benchmark was obtained. The calculations below
describe logical texture traffic, not observed DRAM bandwidth or promised
frame-time savings.

## What owning this stack makes possible

EDVR controls the OpenVR compatibility interface, game-side D3D11 hooks and
postprocessing, the cross-device transfer, and the OpenXR application's frame
loop and swapchains. We can agree on resource lifetimes and combine work across
these boundaries instead of passing an opaque eye texture between independent
products.

Elite still controls its engine, and the selected OpenXR runtime controls
distortion, reprojection, display scheduling and, where applicable, streaming.
Native OpenXR does not give EDVR control of those internals. Preserve the
Windows-selected runtime and use standard OpenXR/D3D11 capabilities; the
recommendations do not require a vendor SDK.

Keep the two-device architecture. It was introduced after the game stopped
servicing render callbacks during shutdown, leaving borrowed-device runtime
teardown unable to finish. The [shared-device
investigation](openxr-shared-device-2026-09-13.md) and [shutdown
evidence](openxr-shutdown-lifetime-2026-09-13.md) explain the constraint. A
return to the game device as the runtime binding would reopen that problem
unless a new ownership design first resolves it.

## Current steady-state work

For a rendered, non-withheld stereo pair on the separate-device path:

1. `waitPoses` waits/begins the XR frame, locates geometry and publishes the
   projection/temporal inputs.
2. Each game `Submit` synchronously enters the XR owner. That owner sends
   game-device work back to the render caller: FSS treatment, temporal AA/DLSS,
   sharpening and EDVR menu composition, when their providers are acquired. A
   provider may immediately pass through, but its dispatch has already
   happened.
3. The producer copies the selected texture into a shared texture, flushes and
   releases its keyed mutex. The XR owner acquires the shared texture, copies
   it into a private texture, flushes and releases the mutex.
4. After both eyes arrive, the compositor creates two shader-resource views,
   acquires/waits each runtime image, records a fullscreen draw into a deferred
   context, executes it with state restoration, flushes and releases the image.
5. `xrEndFrame` submits the projection layer. Successful resubmission retention
   swaps capture buffers; it does not copy another pair of images.

Source: [host capture/submission](../src/openxr/native_runtime_host.h), lines
501–624 and 633–902; [transfer](../src/openxr/shared_texture_transfer.cpp),
lines 257–307; [final rendering](../src/openxr/d3d11_stereo.cpp), lines
394–471.

| Operation | Count per ordinary stereo pair | Qualification |
| --- | ---: | --- |
| Producer-to-shared full-resource copies | 2 | Selected postprocessing output, or raw submitted resource |
| Shared-to-private full-resource copies | 2 | XR device; not runtime compositor work |
| Explicit transfer `Flush` calls | 4 | Two producer, two consumer |
| Final fullscreen draws / command lists / explicit flushes | 2 each | Separate-device scene composition |
| Newly created final scene SRVs | 2 | Even when capture textures are reused |
| Graphics callbacks | 12 on the all-provider, non-healed, GPU-timed path | Six per eye: four treatments, transfer producer, ending GPU marker; allocation/startup callbacks excluded |

These counts exclude AA/DLSS's internal work, optional source-view copies, FSS
snapshots, and menu/sharpening intermediates. They are not a total pass count
for Elite. `FrameBoundary` already avoids pixel treatment when the frame should
not render.

For an illustrative 4404 × 4348 RGBA8 eye, one image is 76,594,368 bytes, or
73.05 MiB. Four full-image copies entail eight image-sized reads/writes per
stereo pair: 584.37 MiB, equivalent to 55.15 GB/s at 90 pairs/s. Removing the
two consumer copies removes half that logical traffic, 27.57 GB/s at that
size/rate. Caching, compression, overlap, scheduling and source dimensions
determine the actual benefit. This size is an example used by the [earlier DLSS
review](review-dlss-performance-2026-09-10.md), not a measurement of the
current installation.

## Ranked opportunities

Priority here means implementation order, not a measured speedup ranking. Batch
the qualification work so the user does not need a headset session for every
small change.

| Priority | Change | Expected target | Confidence / scope |
| --- | --- | --- | --- |
| 1 | Cache final SRVs; simplify XR-owned draw/state work | Driver/CPU overhead | Work is visibly repeated; small local changes |
| 1 | Combine game-side treatment callbacks | CPU scheduling and submission latency | Repeated round trips confirmed; preserve provider semantics |
| 2 | Separate producer publication from consumer acquisition | Time spent blocking the render caller | Architectural hypothesis; instrument the wait first |
| 2 | Supply runtime visibility masks through the shim | Elite scene pixel shading | Missing capability confirmed; runtime support and Elite consumption unverified |
| 3 | Sample shared eyes without the private copy | Full-resolution bandwidth and memory | Large structural saving; ownership/color redesign required |
| 3 | Write the last producer pass into a shareable output | Another full-resolution copy | Requires coordinated graphics-provider and transfer changes |
| 4 | Move EDVR panel composition to the XR side | Extra scene copies while panel/monitor is visible | Conditional benefit; preserve curved-panel behavior |

### 1. Remove repeated work in the final XR renderer

`D3D11Stereo::renderCaptured` creates two SRVs every pair at lines 400–426,
although transfer allocations are already reused when dimensions and format
family are unchanged. Cache views with the capture slot/resource generation and
view format, including gamma versus linear interpretation. Hold proper COM
ownership, retire the cache on resize/close, and account for
`captured.exchangeBuffers(previousPair)` swapping slot ownership. Caching only
a raw pointer or eye index is insufficient.

The scene blit also clears the target at line 456 immediately before a
fullscreen triangle. Its shader has no discard and writes the whole viewport.
Remove this clear only for the proven full-coverage scene path; retain clears
where diagnostics, masks or partial rendering need initialized pixels. The
clear may be cheap on a particular driver, so measure rather than count it as a
full DRAM write.

The separate-device path still uses `FinishCommandList(FALSE)` followed by
`ExecuteCommandList(..., TRUE)` at lines 120–153. State restoration exists to
protect a borrowed game context, but this path owns a separate immediate
context. First benchmark execution without state restoration, rebinding
required state. Then evaluate direct immediate rendering on the XR owner,
removing deferred recording/playback entirely for this mode. Keep
borrowed-device fixtures' state-preservation contract intact, and explicitly
bind state after runtime calls rather than assuming the runtime preserves it.
Microsoft's [command-list
documentation](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-executecommandlist)
identifies avoidable state transitions with restoration enabled.

One command list containing both eyes is an alternative if retaining deferred
rendering is useful. Acquire/wait both destinations and finish all their GPU
work before releasing them; retain correct cleanup if the second eye fails.
Compare that with the simpler immediate path, rather than layering both changes
together initially.

**Validation:** steady-state SRV creation reaches zero after warmup; output
matches for RGBA/BGRA, gamma/linear, crops, flipped bounds and resize; measure
CPU recording/execute time and XR compose GPU time separately. Preserve
device-removal handling, GPU retirement and clean shutdown. No image-quality
tradeoff is intended.

### 2. Combine the game-side treatment callbacks

The host independently invokes graphics work at lines 763, 775, 820 and 842.
Transfer publication invokes again in `SharedTextureTransfer`, and line 658
invokes only to end the producer timing marker. Each [dispatcher
invocation](../src/openxr/render_thread_dispatcher.h), lines 44–61, allocates a
`GraphicsJob`, locks shared state, wakes the render caller and waits for its
response.

Create one per-eye treatment operation containing FSS, temporal treatment,
sharpening and menu work in their existing order. Carry the frame inputs
explicitly and return the selected texture, bounds and treatment outcome
together. This reduces the six callbacks to three on the path counted above
without yet redesigning transfer or timing. It also permits one validated
source interface to be passed through the internal operation instead of
repeatedly querying it.

Do not simply omit callbacks for disabled settings: temporal bookkeeping, eye
dumps, once-per-frame settings, sequence consumption and history invalidation
still run on passthrough paths. Preserve these semantics inside the combined
operation. AA precedes sharpening; EDVR text follows sharpening; raw jittered
pixels retain their actual projection if temporal treatment refuses.

The current transfer deliberately schedules its own producer callback and must
not be called from inside that callback. A later combined
treatment-and-publication API needs an explicit producer half, prepared on the
XR owner, followed by consumer work on the owner. Nesting today's synchronous
`captured.capture` in a treatment callback can deadlock.

Coalescing the ending GPU marker is a separate measurement-contract change:
today the marker is issued after `boundary.submit`, which on the second eye
includes composition and `xrEndFrame`. Moving it earlier changes what the
reported elapsed interval includes. Separate issuing the timestamp from
accepting/invalidation of the completed frame, and document any metric boundary
change.

**Validation:** count callbacks per pair and measure queue wait versus callback
execution. A/B CPU submission p50/p95/p99 with GPU timing both on and off.
Preserve wrong-thread rejection, cancellation, no callbacks after admission
closes, missing/duplicate-eye behavior and live settings. Pooling queue
allocations can follow if profiling still identifies them; avoid replacing the
lifetime protocol merely to save small allocations.

### 3. Stop waiting for consumer acquisition inside the first eye's Submit

`SharedTextureTransfer::producerCopy` immediately calls `consume` at line 281;
`consume` waits in `AcquireSync(1, timeoutMs)` at line 286. `EyeCapture` gives
this operation a 100 ms bound at line 201. This is not a fixed 100 ms cost, but
any actual wait occurs while the submitting render caller is held in its
synchronous rendezvous.

Split producer publication from consumer acquisition. Before returning from
each Submit, enqueue the source copy on the owning game context and publish an
EDVR-owned shared slot, preserving ordering before Elite can reuse its texture.
Acquire/consume the shared eyes when composing the complete pair. Never return
while merely retaining an AddRef to mutable game pixels for a later copy.

This can let Elite continue CPU work after the first eye instead of waiting for
its consumer handoff. Whether it overlaps useful rendering depends on Elite's
actual submit order and the driver's scheduling; both eyes may already have
been rendered. Log the order and wait durations before treating this as a gain.
The second eye still must finish the real frame deadline. Do not build a deeper
queue that quietly adds a frame of latency.

Use a bounded slot protocol for publication, pending consumer ownership,
complete-pair acceptance and retirement. Clear/missing-eye/shutdown paths must
resolve pending slots without requesting a new game callback. Buffering cannot
weaken the stopped-Present teardown guarantee.

**Validation:** separately time producer acquire, producer copy submission,
consumer acquire, and both flush calls. Use a realistic-size two-device fixture
plus the existing overwrite-after-submit test. A win is reduced critical-path
wall time without worse GPU completion, latency, pair consistency or exit
behavior.

### 4. Restore hidden-area masking using standard OpenXR

`OpenVRSystem::GetHiddenAreaMesh` returns an empty mesh at [line
255](../src/openxr/openvr_system.cpp), and the host's instance extension list
at lines 1173–1178 does not enable `XR_KHR_visibility_mask`. The historical
[OpenVR interface](../src/openvr/compat/openvr_v0_9_20.h), lines 1112–1118,
explicitly offers a mesh for early rejection before eye shading.

When the selected runtime advertises
[XR_KHR_visibility_mask](https://raw.githubusercontent.com/KhronosGroup/OpenXR-Docs/main/specification/sources/chapters/extensions/khr/khr_visibility_mask.adoc),
retrieve each eye's hidden triangles, translate them into the legacy mesh
representation, and publish immutable storage with a safe lifetime. OpenXR
supplies coordinates on the view's z = −1 plane, requiring projection; they are
not already the legacy mesh's texture coordinates. Handle per-view mask-change
events and absent masks.

First instrument whether Elite calls this method and actually draws the
returned mesh. Empty-return behavior currently has no call counter here. If
Elite consumes it, savings may reach scene shaders before submission. If it
does not, applying a mask only to EDVR's final blit saves only that blit;
modifying game depth/stencil through hooks would be a separate, more invasive
proposal.

Projection conversion must include asymmetric/canted eyes and the actual
advertised game frustum. Use conservative coverage around temporal jitter and
filtering. Do not send a cached mask for a mismatched FOV or experimental crop,
and do not claim that hiding corners reduces DLSS compute unless its work is
explicitly made mask-aware. Keep rendering normally when this optional
extension or a usable mask is absent; that does not select a legacy VR backend.

**Validation:** record extension support, mesh sizes, shim call count and a
verified game mask draw. Compare uncovered visible pixels and GPU scene cost at
fixed resolution. Test runtime mask updates, recentering, head movement and AA
edges on Pimax and Quest runtimes. No visible cropping is acceptable.

### 5. Remove the consumer private copy

Currently, each shared image is copied into an XR-private typeless texture
before it can be sampled. A redesigned compositor could sample the shared
consumer texture while holding its ownership lease, then release it once all
consuming commands have been submitted with the required synchronization. This
removes one full copy per eye and its private allocation, while keeping runtime
resources on the independent XR device.

The current [transfer resource
description](../src/openxr/shared_texture_transfer.cpp), lines 163–175, uses
typed UNORM for sharing and typeless private storage. That choice supports the
compositor's linear or sRGB views. Direct sampling therefore needs a validated
format/view strategy that preserves decoding before filtering. Sampling encoded
UNORM and decoding after bilinear filtering is not automatically equivalent.
Test actual format sharing/view support across the supported devices; do not
assume a shared handle permits arbitrary reinterpretation.

Preserve the last accepted pair for resubmission without producer overwrite.
Its resources and original pose/FOV/reference metadata must stay together. A
bounded pair of shared slot generations may replace private retention, but the
lease protocol must cover every repeated read. Never hold an acquired runtime
swapchain image indefinitely to implement history retention.

The change also modifies the GPU timing phases: a removed copy must not keep
reporting old `XR COPY` samples. Measure transfer synchronization independently
from the final draw and report unavailable/changed fields honestly.

**Validation:** an isolated full-resolution direct-sampling benchmark must
establish both image equivalence and lower total submission cost. Stress
producer overwrite, delayed consumer, timeouts, retained-pair replay, resize,
device loss and shutdown after Present stops. A longer mutex hold can erase the
bandwidth saving; compare end-to-end timing as well as the removed GPU
interval.

### 6. Make the final producer output shareable

EDVR owns the AA/sharpen/menu outputs. Instead of rendering into an ordinary
intermediate and then copying it into shared storage, an eligible final pass
could write directly into a prepared shared slot. This targets the producer
copy too; it is a subsequent step, not a prerequisite for the lower-risk
callback work.

For example, sharpening produces `e.outTex` in
[sharpen_pass.cpp](../src/d3d11/sharpen_pass.cpp), lines 543–614. With a
visible panel, [menu_panel.cpp](../src/d3d11/menu_panel.cpp), lines 1328–1347,
already copies the scene into another output before compositing its box. A
destination-aware API can choose the final writer based on the frame's actual
enabled treatments rather than introduce a new pass.

This requires producer-side ownership acquisition before UAV/RTV writes,
supported shared view formats, and exclusion from game render-target hooks. The
current shared resource deliberately has no RTV bind flag because game hooks
can rewrite matching dimensions. Do not casually add it. Raw passthrough still
needs a source snapshot, and DLSS's temporal/history resources cannot simply be
reassigned to a slot the consumer may own.

**Validation:** identify the final writer for every feature combination,
preserve input/output aliasing rules, and count removed copies. Only adopt
shared destinations where their lifetime and hook classification are proven.
This is a quality-preserving transport optimization, not permission to alter
DLSS settings.

### 7. Avoid copying the whole scene to display EDVR's panel

The menu path already skips composition when the panel is hidden or offscreen.
When visible, its ordinary path copies the full eye region before dispatching
only the panel's box. The floating monitor makes this relevant outside the F8
settings menu.

Share the comparatively small panel texture when its raster changes, and draw
it on the XR device after the scene blit. Reuse the existing per-eye geometry,
alpha, curvature and anchor rules. This can remove a full scene copy per eye
during panel use and decouple display of EDVR UI from game postprocessing. The
panel update transfer is new work and must be included in the comparison.

An OpenXR quad layer is another option for genuinely flat EDVR UI, but
replacing a curved panel with a flat layer would change behavior. Do not make
that substitution as a performance fix. Neither approach automatically extracts
Elite's own cockpit/menu text; this finding concerns EDVR's panel and monitor.

**Validation:** compare menu hidden, monitor visible, and settings visible;
check text, blending, scale, world anchoring and eye dumps. An XR-side panel
needs a deliberate dump composition path so diagnostic images still represent
what the user sees.

## Measurement needed before larger changes

The [current timing contract](openxr-device-timing-2026-09-14.md) is useful,
but `XR COPY` covers the consumer copy only, and `XR COMPOSE` covers
command-list GPU execution. Neither measures keyed-mutex CPU waits or the
runtime compositor. The host's CPU fields include rendezvous and runtime waits;
`composeMs` is also contained in the second Submit. Do not add overlapping
fields or call them exclusive CPU time.

Add one bounded diagnostic capture with frame sequence, resource
dimensions/formats, producer callback counts and wait/execution durations,
producer/consumer acquire durations, flush CPU duration, swapchain acquire/wait
duration, and `xrEndFrame` duration. Add a producer-copy GPU interval to
distinguish transport cost from the existing broad render interval. Keep
per-device GPU clocks separate; a timestamp span may include idle gaps, and
summing overlapping device spans is not total GPU busy time.

Use asynchronous query rings and aggregate p50/p95/p99 over a controlled
window. The existing timing logs are throttled to five-second snapshots, not
frame-time distributions. Do not introduce per-frame synchronous file writes or
blocking query reads. The current query ring already reuses successful slots
and polls pending results without forcing completion.

For comparisons, record build/DLL verification, runtime/version,
headset/connection, refresh rate, actual submitted and swapchain dimensions,
render percentage, DLSS mode/version, scene, menu/monitor state and runtime
reprojection settings. Match dimensions rather than percentages across
runtimes. Repeat A/B runs after warmup and compare frame deadlines and latency
as well as median time. A faster submission path may increase time spent
waiting in `xrWaitFrame`; that alone is not a regression.

## Changes not justified by this review

- Do not remove `xrWaitFrame`, invent extra predicted frames, or substitute
  arbitrary newer poses for the pose that rendered an image. OpenXR [frame
  pacing](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrWaitFrame.html)
  permits structured pipelining but requires consistent frame timing.
  Investigate it only after identifying an actual scheduling bottleneck.
- Do not delete all flushes or replace shutdown completion with a flush.
  Microsoft's [Flush
  contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-flush)
  distinguishes command submission from completion. Six explicit flushes per
  pair justify measurement and possible batching, but inter-device visibility
  and image-release ordering still need to hold.
- The transfer's event-query drain is reached for changed allocations and
  shutdown, not every same-size frame. Its flags and bounds were chosen after
  an observed driver issue. It is not a routine hot-loop stall to remove.
- Previous-frame retention already swaps resources instead of copying pixels.
  Do not claim another two-copy saving there.
- The owner's five-millisecond idle wait wakes on queue notification. It does
  not impose five milliseconds on every request. Wholesale lock removal would
  threaten the teardown/lifetime guarantees.
- Render settings and sizing publication are startup work here. Caching their
  `GetProcAddress` calls will not materially improve steady-state frame time.
  Native trace and timing reports are not unconditionally emitted every frame
  either.
- Respect the runtime's preferred ordering among equally suitable swapchain
  formats when evaluating format choices, while preserving the color contract.
  [OpenXR recommends the highest supported runtime
  preference](https://raw.githubusercontent.com/KhronosGroup/OpenXR-Docs/main/specification/sources/chapters/rendering.adoc).
  There is no current evidence that RGBA versus BGRA selection explains the
  reported slowdown.
- Automatic resolution changes, VRS and [foveated
  DLSS](foveated-dlss-design-2026-09-14.md) are separate quality/budget work.
  They should not conceal transport overhead or reopen the head-motion blur
  regression. No experimental feature is required for the immediate
  recommendations.

## Suggested implementation and qualification sequence

1. Add the narrowly scoped diagnostic window, SRV cache, full-coverage clear
   optimization and batched treatment callback, keeping each change
   independently comparable. Benchmark the XR-owned draw without state
   restoration and with direct immediate submission in the desktop harness.
2. In parallel in the same test build, establish visibility-mask support and
   Elite's consumption of the legacy mesh call. Do not promise scene-rendering
   savings before that evidence.
3. Use full-size two-device fixtures to compare deferred consumer acquisition
   and direct shared sampling. Advance only if they preserve source
   snapshotting, color and independent shutdown while improving the complete
   frame path.
4. Once transport is settled, evaluate shareable final producer outputs and
   XR-side EDVR panels. Keep foveated rendering on its own design/quality
   track.

For any eventual C++ changes, run the full build and relevant existing
fixtures: `openxr_render_thread_test`, `openxr_shared_texture_test`,
`openxr_capture_test`, `openxr_stereo_test`, `openxr_frame_test`,
`native_device_gpu_test`, `native_temporal_test`, `native_menu_test`, and
shutdown/module coverage. Extend only the affected contracts. Desktop
correctness tests are not a substitute for full-resolution measurements or
headset validation.

One combined headset checklist should cover fixed-resolution menu, cockpit and
on-foot scenes; still and moving head with DLSS/TAA; sharpening and
panel/monitor toggles; eye dumps; recentering and a resolution restart; and
normal exit plus stopped-Present teardown. Repeat on the Windows-selected
SteamVR OpenXR/Pimax path and the available Meta/VDXR Quest paths. Preserve
image quality, startup orientation and exit correctness before accepting any
timing improvement.

## 2026-09-24: issue #38 and the five gaps

Issue #38 (Bigscreen Beyond 2e on SteamVR, RTX 5090) reported random
frame-time jumps in 0.17.0 against a steady 0.16.2 and blamed OpenXR. The
logs could not test it: every Pimax-on-SteamVR-OpenXR flight is 4 minutes or
shorter, the legacy and native logs share no frame-time distribution, and a
native LONG FRAME line printed no breakdown.

Measured from existing logs, no flight:

- The owner handoff (round trip minus owner body, `native_frame_cycle_phase`)
  costs 10-60 us at p50 and p95.
- Elite's thread sits in the second Submit (`second_submit_render_park`, p50)
  for 0.59 ms on Pimax OpenXR at 2600x2514, 0.76 ms on SteamVR at 4100x4050
  and 1.26 ms on SteamVR GPU-bound: consumer copy, compose and the runtime's
  xrEndFrame.
- SteamVR's xrEndFrame (`native_submit_phases`, p50) is 0.12 ms at 2665x2087
  and 0.46-0.96 ms at 4100x4050; Pimax OpenXR's is 0.23-0.46 ms.
- EDVR-device GPU (consumer copy plus compose) is 0.05-0.07 ms median over 66
  flights. The producer copy on Elite's device was not timed.
- Every flight runs `graphics_ownership,mode=separate`. Nothing in `src` set
  a thread priority.

Built on main, not flown:

- `f12a5a3f`: `native_long_cycle`, one line per cycle longer than twice the
  predicted period, with that cycle's phases and the timing sequence the
  d3d11 LONG FRAME line now also prints ("runtime sequence"). p99/max on the
  phase lines; `late_frames` per window and session; XR_EXT_performance_
  settings and XR_META_performance_metrics when the runtime lists them.
- `e5be5c2b`: `frame_end_overlap` and `frame_thread_priority`, both on by
  default. With the overlap, the second Submit returns once Elite's texture
  is free, and the owner finishes the pair from a job queued ahead of the
  next WaitGetPoses. Turbo pacing and the borrowed device stay synchronous.
- `57119f42`: `native_producer_gpu`, the producer copy's GPU time on Elite's
  device, every 30 s.

Both switches live in `Openvr\win64\edvr_openxr.ini`, which is all or
nothing: a file that sets them must restate the packaged defaults.

    [openxr]
    version=1
    loader=<game>\Openvr\win64\openxr_loader.dll
    graphics=<game>\d3d11.dll
    runtime=system
    separate_device=1
    frame_end_overlap=off
    frame_thread_priority=normal

Why the copies exist: the write into the runtime's swapchain image is
inherent to OpenVR over OpenXR (SteamVR's own Submit copied too). The
cross-device copy buys the separate XR device the shutdown work needed
([shared device](openxr-shared-device-2026-09-13.md)). The private copy gives
compose a typeless view and returns the keyed mutex at once.

What the flight reads (the `edvr_openxr` log):

- `native_frame_end_overlap,enabled=1` at startup, and its `_summary` at
  close with `failures=0`.
- `second_submit_render_park` p50 should fall to the producer part, about
  0.2 ms. `frame_end_owner_body` carries what moved; `next_wait_queue_delay`
  near zero means the wait did not simply move to the next WaitGetPoses.
- `native_thread_priority,thread=owner,...,after=2` (the pacer line appears
  only on foot).
- `native_producer_gpu,window=` copy p50: above about 0.1 ms per eye reopens
  sections 5 and 6. A few `pending_dropped` are spans still in flight when a
  window closed.
- `native_long_cycle` and `late_frames`: the per-frame breakdown issue #38
  needs.

Ruled out:

- ruled out: SteamVR's OpenXR runtime collapsing to about 50 fps two minutes
  in (Frontier `aapvr` flights 20260922_093752 and 20260922_125854), because
  at the drop the game's own GPU render went from 1.2 to 15-19 ms and Game
  Map writes from about 15k to 800k per 5 s: the settlement loading in,
  GPU-bound.
- ruled out: recent main having more long frames than 0.17.0, because the
  LONG FRAME count is capped at one per 5 s and the main flights were 3-4
  minutes, mostly load-in; with load-in and streaming removed, neither flight
  long enough to test kept a clean long frame.
- ruled out: Vulkan, DXVK or Khronos loader cost (issue #38), because the
  runtime enables only XR_KHR_D3D11_enable and uses the loader once, at
  startup.
- ruled out: the owner rendezvous as a cost, because it measures 10-60 us.

## 2026-09-24: flights 103456 and 104337

Frontier install, build `74255330`, Pimax Crystal Super on SteamVR OpenXR
(`aapvr`), 4100x3962 out, DLSS quality (2665x2575 in), HMD Quality 0.65.
Sean reported DLSS broken, no GPU frame time, and the loading-screen
hologram fix not working. The second flight, with Elite's Supersampling
back at 1.0, restored DLSS and the hologram; the GPU frame time stayed
broken.

- The overlap works as a performance change: `second_submit_render_park`
  p50 0.17-0.24 ms (0.76 ms before at 4100x4050), `next_wait_queue_delay`
  p50 0.006 ms, so the wait did not move to the next WaitGetPoses.
- It breaks GPU timing. Elite's thread resumes the producer and reopens an
  application segment right after the second Submit (native_runtime_host.h,
  the caller path); in the synchronous path the owner had already published
  and retired the frame, so both were no-ops. With the finish queued, they
  land on a live sequence and the Application-render GPU ring rejects the
  frame's segments: `old sequence`, valid 8725 invalid 43706 (104337),
  against invalid 2-3 on the older builds; the Monitor benchmark reads
  `cpu --/--/-- valid 0`. The overlap now defaults off.
- `frame_end_owner_body` reads 0 at p50: the queued job usually runs before
  Elite's thread records its second eye, which the stats require.
- `native_producer_gpu` copy p50 0.039 ms, p99 0.34-1.7 ms in steady windows
  (the first window, loading in, 1.15 ms). Under the 0.1 ms bar: sections 5
  and 6 are closed.
- SteamVR's OpenXR runtime offers XR_META_performance_metrics
  (`/perfmetrics_meta/app/gpu_frametime`, 3.8-6.1 ms by window) and not
  XR_EXT_performance_settings.
- `late_frames` 600 and `native_long_cycle` 50 in the 103456 session.

Ruled out:

- ruled out: this build breaking DLSS or the hologram fix, because Elite's
  Supersampling (`SSAAMultiplier`) was below 1 in flight 103456: the world
  was drawn into 1998x1931, 75% of the 2665x2575 eye texture ("busiest render
  target" line), so DLSS got an upscaled image and the hologram draw never
  matched; with Supersampling 1.0 (104337) both work on the same build.
- ruled out: the producer-copy queries as the GPU timing fault, because the
  rejected samples read `old sequence`, never `disjoint`.

## 2026-09-24: overlap timing fix implemented, not yet flown

Branch `claude/openxr-perf-gaps`. Addresses this doc's Open item above.

- submitEye's deferred branch now publishes the frame's CPU record and
  retires timingFrameActive/timingApplicationOpen/timingApplicationSequence/
  timingGpuBegun itself, before Submit returns to Elite -- the same state
  the synchronous path already leaves by then (publishSubmitTimingCpu). Only
  what finishPair() actually produces (device GPU spans, the consumer/
  endFrame/wait/pacer submitSample fields, the final timingSequence/
  timingFrameMask clear) stays in the queued job (publishSubmitTimingDevice,
  called from the new finishPendingFrameEndBody, shared with close()'s
  inline fallback). publishSubmitTimingIfComplete, the synchronous path, is
  untouched. composeMs, the one CPU-record field finishPair() itself
  produces, is a frame stale under the overlap: the deferred publish carries
  the previous overlapped pair's measured value (lastOverlappedComposeMs).
- frameEndOwnerBegin/End (frame_cycle_stats.h) moved off current_.eyes==2:
  the queued job usually finishes before the caller's own second-eye
  submitCallerEnd lands, so the bracket is now a pending value tracked
  outside current_, consumed by finishCurrent() into whichever cycle is
  closing.
- New tests: openxr_native_test's frame_end_overlap_cases.h drives a real
  deferred pair through submitEye's own caller/owner split with a fake
  EdvrNativeTimingTable, asserting publishCpu runs before the caller's
  post-Submit producerResume/applicationSegment(seq,true) and that those
  then find it retired, not reopened; openxr_frame_test's
  frameEndOwnerBodyRaceTest reproduces the eye-count race directly.
  build.bat green (297 + 4811 checks, both new tests passing).
- Still defaults off (`frame_end_overlap`): this closes the two symptoms
  above, not a decision to ship it on. Needs the flight this doc's Next
  flight line already names.

## 2026-09-24: flight 124504, the overlap flown clean

Frontier install, build `58b6c085`, Pimax Crystal Super on Pimax OpenXR,
`frame_end_overlap=on` added to the installed edvr_openxr.ini. Sean: GPU
frame times work again.

- `native_frame_end_overlap_summary`: overlapped 25545, synchronous 0,
  failures 0.
- Application-render GPU: valid 25304, invalid 5 (43706 invalid before the
  fix); the Monitor benchmark reads cpu p50/p95/p99 3.32/4.13/4.36 ms.
- `second_submit_render_park` p50 0.16-0.18 ms, p99 0.35-1.12;
  `frame_end_owner_body` p50 0.29-0.32 ms, now recorded;
  `next_wait_queue_delay` p50 0.005-0.006 ms, p99 at most 0.06: the moved
  work did not come back as a wait.

The overlap now defaults on.

## 2026-09-24: flight 125717, the overlap on SteamVR OpenXR

Same build and switch, Pimax Crystal Super on SteamVR OpenXR (`aapvr`).

- Overlapped 9937, synchronous 0, failures 0. Application-render GPU valid
  9625, invalid 4.
- `second_submit_render_park` p50 0.16-0.19 ms, p99 0.37-0.44 (0.76 ms p50
  before on SteamVR at 4100x4050).
- `frame_end_owner_body` p50 0.62-0.70 ms, p95 0.9-3.4, p99 1.1-7.8: this
  is mostly SteamVR's own xrEndFrame, and before the overlap all of it sat
  on Elite's render thread inside the second Submit. Now it finishes before
  Elite's next WaitGetPoses (`next_wait_queue_delay` p99 0.011-0.030 ms).
- For issue #38 (random CPU frame-time jumps on SteamVR in 0.17.0): that
  xrEndFrame tail on Elite's thread is a plausible contributor, and this
  build takes it off. Not proven as the reporter's cause.

## 2026-09-25: issue #38 on rc.1, and the EDVR GPU census

The reporter tested v0.18.0-rc.1 (comment 5830530459). Frame time is "more
stable than 0.17.0", but GPU utilization reads 90-99% against 0.16.2's
60-62%. Setup: the "Basic Flight" tutorial, SteamVR 3560x3560 per eye at
75 Hz, HMD quality 0.75, DLSS preset L, an RTX 5090. No logs.

That comparison spans the runtime switch AND every GPU feature EDVR added
between 0.16.2 and rc.1: the motion paths, UI depth and its content
tracking, and the hologram depth pass. Sean's own rig reads a native
benchmark gpu p50 of about 10 ms at 90 Hz on the same GPU class. The only
runtime-side costs measured are small: the producer copy, 0.039 ms p50 per
eye, and the compose.

To split EDVR from the game, `src\d3d11\gpu_census.*` now logs one line
every 30 s:

    EDVR GPU census: 30 s, N frames; EDVR ~T ms/frame = door D (...) +
    in-frame F (...); application render p50 R ms/frame (game ~G); ...

- **Sections:** each of 19 wraps a call site. The door sections are the
  whole temporal pass, the upscaler, motion prep, the hologram resolve with
  the celestial views, the UI resolve, sharpen, menu, the UI layer
  composite and FSS heal. The in-frame sections are the hologram passes,
  UI depth coverage, planet, terrain, screen, weapon and engine motion, the
  UI layer reissues, eye mask and foveation.
- **Sampling:** one section is timed per frame, round-robin, because only
  about 31 timer spans fit per frame. It times at most 4 calls at the door
  and 8 per draw, strided across the frame from a rotating offset, since
  per-draw costs vary. Its cost is the mean ms per timed call times calls
  per frame.
- **R:** the Application-render GPU median over the window, covering the
  game plus EDVR on the game's device. The XR device's transfer and compose
  are outside it; `native_producer_gpu` reports the transfer.
- **Rig:** `tools\gpu_census_test`.
- **Next:** any flight on this build gives EDVR's share of the frame. A
  SteamVR flight in the Basic Flight tutorial matches the reporter's scene.

**First census, flight 20260925_080452** (e52089de; Pimax OpenXR, DLSS
2037x1969 -> 4074x3938 per eye, 90 Hz, RTX 5090). Four 30 s windows in
flight:

- EDVR ~4.6-6.7 ms/frame.
- Door 4.0-5.2 ms:
  - upscaler 3.1-4.1 ms, both eyes;
  - UI resolve 0.30-0.82 ms;
  - motion prep 0.13-0.35 ms;
  - UI layer composite up to 0.42 ms;
  - hologram resolve and celestial up to 0.09 ms.
- In-frame up to 2.6 ms in the busier windows:
  - engine velocity 0.3-1.45 ms, at 524-707 calls/frame;
  - UI depth coverage 0.75-1.14 ms, at 39-68 calls/frame;
  - hologram passes 0.33-0.40 ms;
  - screen motion about 0.1 ms.
- Application render p50 5.6-8.7 ms/frame, so the game is roughly
  1-2.7 ms.

On this rig EDVR's own work is about two thirds of the frame's GPU time on
the game's device, and the upscaler is the largest item. The #38 gap,
0.16.2 at 60% against rc.1 at 95%, is therefore far more likely to be
EDVR's features than the runtime. The next step is the reporter's census
line.

## 2026-09-25: the census's own per-draw overcount, and the fix

Flight 20260925_081429 (same rig) read EDVR ~8.0 ms/frame against an
application-render p50 of 7.08 ms -- "game ~-0.961", negative. Engine
velocity read 3.282 ms at 306 calls/frame in one window, then 0.171 ms at
552 calls/frame in the next: more calls, an order of magnitude less cost,
the signature of a timer measuring itself rather than the work.

**Cause:** `FrameEngineVelocity` and `FrameScreenMotion` wrapped the CALL
SITE (`engineVelocityBeforeDraw`; `screenMotionUiDraw`/`screenMotionDraw`),
not the GPU work inside it. Both mostly return without issuing anything --
engine velocity's inline fast path skips its slow path unless something was
rebound since the last draw; screen motion returns above g.sourceFrame!=
g.frame and several shader/target checks. Timing the call site mostly timed
the begin/end pair's own pipeline-drain cost, hundreds of times a frame.

**Fix** (this branch, `claude/openxr-perf-gaps`):
- The wraps moved to where GPU work actually issues:
  `src\d3d11\engine_velocity.cpp`'s eye-frame clear (`slowPath`), pool+scene
  snapshot copy (`snapshot`) and append refresh copy (`checkSources`);
  `src\d3d11\screen_motion.cpp`'s UI mask clear+reissue
  (`screenMotionUiDraw`), the eye's buffer/size copies, clear and
  projection draw (`screenMotionDraw`), and the frame-boundary panel-count
  readback (`flushPanelCounts`, found beyond the two named functions by the
  same file-wide sweep). The old wraps at `vscreen.cpp`'s seven
  `engineVelocityBeforeDraw` call sites and its one screen-motion site are
  gone; the calls themselves stay.
- Calibration: on a section's turn, its first timed call also times one
  empty begin/end pair (a second `GpuIntervals` per section, `nullSampler`)
  immediately before the real one. The line's ms/frame is now `max(0, timed
  mean - null mean) * occurrences/frame` (`gpu_census.cpp`'s
  `correctedMsPerCall`), so a genuinely tiny operation reads near 0 instead
  of the pair's own overhead.
- The line adds `timer floor X us/pair` (the pooled null mean across every
  section this window) and appends `(census over the frame total)` when
  EDVR's corrected total still exceeds R -- the game's share stays raw and
  negative either way, the point being to show it, not hide it.
- `tools\gpu_census_test` (62 checks, up from 49): the calibration math as
  a pure function, a real WARP round trip of the null pair, and the line's
  new fields. `tools\engine_velocity_test` (`lifecycle_tests.h`) and
  `tools\screen_motion_test` got `gpuCensusBegin`/`End` stubs, since both
  now call them directly and neither links `gpu_census.cpp`.

**Not yet flown.** Next flight: re-run the reporter's SteamVR Basic Flight
scene, or Sean's own rig, on this build. Engine velocity's and screen
motion's ms/frame should stop swinging opposite their calls/frame, and
"game ~..." should stop reading negative on a steady scene. If EDVR's
corrected total is still large, that is now believable rather than an
artifact of the timer.

## 2026-09-25: jntracks, 0.6.0 against 0.17 and 0.18 without temporal AA

A second user (jntracks, via Sean) reads the 90s in fps on EDVR 0.6.0
(d3d11.dll only, Elite on SteamVR's own OpenVR) and the 80s on 0.18, with
temporal AA off. The logs hold two 0.6.0 sessions (no version line, "edvr
d3d11 proxy attached"), two v0.17.0 and one v0.18.0-rc.1 (c9cab91e). The
three newer ones run SteamVR OpenXR (`steamvr-openxr-cv`, 90 Hz).

- Per-eye render size is the same everywhere: 2604x2644 at scale 1.0 in
  the OpenXR logs. The only eye-sized draw the 0.6.0 logs sample is
  2604x2644.
- Temporal AA is off at rest: `native temporal totals: treated=0` in
  123542 and 142551. In 122910 it was switched on for about 28 s of
  testing. Sharpening, eye mask, cull, fov trim and settlement detail
  are off, and the UI panel is smaller than 0.6.0's (2880x1620 against
  3200x1800).
- The one structural difference is the runtime path: EDVR's OpenXR
  runtime onto SteamVR's OpenXR, against Elite on SteamVR's OpenVR. A
  steady 0.17 window reads `native timing CPU: ... submits 1.409 ms ...
  transfer 0.370 compose 0.321 ms`.
- These logs cannot measure the gap. 0.6.0 writes no timing lines at
  all, and the newer sessions' heavy stretches were in different places
  (rc.1's on foot in a settlement, Application-render GPU 9.9-12.8 ms).
- ruled out: resolution, because every session renders 2604x2644 per
  eye; EDVR's GPU features, because all of them are off at rest.
- Next evidence: the same place on both builds, with SteamVR's own
  frame timing (GPU and CPU per frame) for each, and main's census line
  on the new build.

## 2026-09-29: phase 0 flown, and the instrument for the post-submit window

Flight 07:02:41-07:09:00 local, Frontier install, build
`v0.18.0-rc.3-94-g8fee57c2` (both logs' version lines):
`edvr_gfx_20260929_070241.log` and
`edvr_openxr_20260929_070243_242_48304.log`. Environment: EDVR's native
OpenXR runtime onto SteamVR/OpenXR (headset key `steamvr-openxr-aapvr`),
4032x3896 per eye, 90 Hz, native temporal pass on (`treated=66014`; DLSS mode
not read for this entry). Phase 0 (`src\common\periodic_work.h`, architecture
review P1) times each job the frame boundary runs on a timer, so the
`periodic work:` lines can be laid against LONG FRAME lines.

**Phase-0 results** (the gfx log's `periodic work:` lines):

| job | runs | max | note |
|---|---|---|---|
| journal_reglob | 97 | 4.284 ms | every run over 2 ms, typically 2.6-2.7; 1,991-1,992 journal files |
| journal_status | 3,267 | 39.220 ms at 07:08:22.789 | 4 slow: 39.2, 4.3, 4.3, 3.5 |
| frame_cycle_report | 10 | 2.065 ms | |
| luma_round | 1,121 | 0.248 ms | |
| ui_layer_totals | | 1.033 ms | |
| xinput_probe | | 1.559 ms | |
| game_exit_probe | | 0.092 ms | |
| journal_tail | | 0.059 ms | |

The gfx log holds 23 `LONG FRAME` lines (rate-limited; one is the 1,117 ms
startup frame). The runtime counted 63 long cycles (over twice the 11.1 ms
period) and logged 54 (`native_long_cycle_summary,count=63,logged=54`; its
four-a-second limit withheld 9). Two of the 54 coincide with a job:
07:08:22.791, cycle 46.2 ms (gfx 50.4 ms), is the Status.json read;
07:05:15.827, cycle 22.7 ms (gfx 26.4 ms), has a journal re-glob 3 ms away (a
2.6 ms job cannot make 11.6 ms of excess by itself). No other long cycle is
within 60 ms of a re-glob beat or of a slow Status read.

- ruled out: EDVR's periodic render-thread jobs as the main cause of long frames, because only 2 of 54 native long cycles coincide with one (Status.json 39.2 ms at 07:08:22.79; journal re-glob at 07:05:15.83)

**Where the long cycles' time is.** The largest phase of 47 of the 54 logged
long cycles is `post_second_submit_to_next_wait` (`next_wait_roundtrip` 4,
`game_before_first_submit` 3). That window runs from Elite's second Submit
returning to its next WaitGetPoses, and holds Elite's Present, which d3d11.dll
hooks: `hookedPresent` calls the real Present, then the frame boundary (about
sixty timed calls: hotkeys, the journal poll, the menu, the vScreen boundary,
the config poll), then the runtime's render callback.

The same runtime log's own post-submit windows (`native_post_submit_phase`,
12 windows of 30 s, 30,737 cycles, every one a single valid Present) already
bound EDVR's share from above. Window maxima in ms, read with
`tools\edvr_log.py --grep`; each column is that phase's own maximum, so the
rows of one window need not be one cycle:

| window (local, end) | post-submit gap | Submit to Present, outside the hook | Present to next Wait, outside the hook | EDVR hook, after the real Present |
|---|---|---|---|---|
| 1 (07:03:03) | 440.2 | 44.3 | 439.8 | 2.5 |
| 2 (07:03:33) | 49.7 | 47.3 | 10.9 | 3.8 |
| 3 (07:04:03) | 421.5 | 420.7 | 17.7 | 3.2 |
| 4 (07:04:33) | 140.5 | 40.3 | 140.4 | 2.6 |
| 5 (07:05:03) | 82.6 | 6.5 | 82.2 | 2.7 |
| 6 (07:05:33) | 19.0 | 1.4 | 18.8 | 2.7 |
| 7 (07:06:03) | 70.2 | 1.7 | 69.8 | 2.7 |
| 8 (07:06:33) | 12.1 | 3.6 | 8.3 | 2.8 |
| 9 (07:07:03) | 8.1 | 0.9 | 7.8 | 2.8 |
| 10 (07:07:33) | 70.1 | 0.6 | 69.8 | 4.4 |
| 11 (07:08:03) | 7.6 | 0.6 | 7.4 | 2.9 |
| 12 (07:08:33) | 41.9 | 0.6 | 8.0 | 39.4 |

In every window EDVR's hook before the real Present was at most 0.19 ms, the
render callback at most 0.12 ms and the driver's Present at most 0.86 ms. In
windows 1 to 11 the gap's maximum is time outside the hook, after the Present
(1, 4 to 7, 9 to 11), before it (2, 3) or both (8), and EDVR's hook stays at
4.4 ms or less. Most hook maxima are the journal re-glob plus the ordinary
boundary (2.5 to 2.9 ms in windows 1, 4 to 9 and 11, against runs of 2.3 to
2.8 ms, one every 4 s); windows 2 and 3 (3.8, 3.2) sit above that and are not
attributed; window 10's 4.4 ms fits the 4.326 ms Status read at 07:07:20.289,
and window 12's 39.4 ms the 39.220 ms read at 07:08:22.789. So the hook is the
largest part of the gap in one window of 12.

This is a bound by window. It cannot say what the hook cost in each of the 54
cycles, or name a tick inside it. And "outside the hook" is everything on the
render thread that is not the Present hook: Elite's own code, and EDVR's other
hooks in the D3D11 calls Elite makes (the draw hooks' own cost is sampled one
frame in 16; the other hooks' is not timed).

**The instrument (built, gates green, not flown).**

- `native_long_cycle` cuts `post_second_submit_to_next_wait` at Elite's
  Present, per cycle, from the Present trace the runtime already reads
  (`edvrReadNativePresentTrace`, five marks per Present): `pre_present`
  (second Submit return to hook entry), `present_hook` (entry to exit),
  `post_present` (exit to next WaitGetPoses entry), and the hook in four:
  `hook_before_real`, `hook_real_present`, `hook_after_real` (the frame
  boundary and the rest of EDVR's work), `hook_render_callback`. The three sum
  to the phase. `present_split=ok`, or the trace's rejection reason
  (`provider_missing`, `partial_present`, `multiple_present`, ...) with none of
  the numbers, follows the phase. Every existing field keeps its name.
- No interface change. The trace is versioned and size-checked
  (`EDVR_NATIVE_PRESENT_TRACE_VERSION_1`, 928 bytes), so halves built from
  different commits fall back as they always did, and an absent or mismatched
  provider reads as `present_split=provider_missing` or `provider_version`.
  The render-boundary callback carries no timing.
- The gfx LONG FRAME line (native, and the non-native branch used when no EDVR
  runtime is loaded) gains
  `EDVR in this frame: X ms in the Present hook (frame boundary Y ms), Z ms in
  the real Present, draw hooks ~D ms (sampled this frame | held from an earlier
  sampled frame), W ms outside the hook; slowest EDVR ticks: name=ms, ...;`.
  `src\d3d11\frame_ticks.h` keeps one chain of clock reads across
  `hookedPresent`, the boundary and `vScreenFrameBoundary` (about 60 named
  marks a frame, 17 ns each here). It is cut at the monitor's frame edge, so
  the ticks, the real Present and the rest add up to the frame with nothing
  left over; the three slowest ticks are kept per frame, the real Present
  excluded.
- **Correction.** The native line printed no boundary figure, and the
  non-native branch printed `cpuBoundaryMs`, which could only read 0.00:
  `perfMonitorNoteCpu(kCpuBoundary)` writes it at the end of the Present hook,
  after `perfMonitorFrame` (inside the boundary) has already written the line.
  The 2026-09-21 forensics in `docs\engine-render-performance-2026-09-19.md`
  ("EDVR boundary/hook costs ~0.00 ms") read that field, so that reading is
  void.
- Rigs: `native_perf_history_test` (the chain, the partition, the top three,
  odd clocks, the text and its worst-case length against the log's line limit)
  and `openxr_native_test`'s frame-cycle cases (the split by hand, every
  rejection reason, the exact line).

**Next flight.** Same scene as phase 0, five minutes steady, on this build.
Read `native_long_cycle`: `present_split=ok` everywhere, then which of
`pre_present`, `hook_after_real` and `post_present` is the largest. If it is
`hook_after_real` in more than a few, the LONG FRAME lines' slowest ticks name
what; the 5 s gate on those lines will hide some, and an un-gated slow-tick
line is the next step if it does. If it is `pre_present` or `post_present`, the
window is Elite's and this arc moves off EDVR's hook.

## 2026-09-29: flight 090608, the split flown at a busy fleet carrier

Build v0.18.0-rc.3-108-gc1408551 on the Frontier install: SteamVR OpenXR
(`aapvr`), 4032x3896 out and 2016x1948 in per eye (Elite's HMD Image Quality
0.5), 90 Hz, DLSS performance mode with preset K, runtime pacing,
`frame_end_overlap` on. Logs `edvr_gfx_20260929_090608.log` and
`edvr_openxr_20260929_090610_039_12932.log`; the busy part is about two and
a half minutes beside Sean's fleet carrier with many ships about
(09:08-09:10:30). Sean, reading fpsVR: clearly better than before, still
about 10.8 ms CPU and GPU with frequent spikes over 11.1; the same scene on
0.16.2 under SteamOS read a similar GPU time and 5.4-6.2 ms CPU.

- **The split.** `present_split=ok` on all 70 logged long cycles (over twice
  the period). EDVR's hook work, before plus after the real Present, totals
  7.4 ms across all 70 and never reaches 2 ms in one; the real Present
  totals 4.5 ms. The time is `pre_present` (1,631 ms, 36%) and
  `post_present` (1,594 ms, 35%), then `first_submit_roundtrip` (773 ms,
  17%), nearly all of it one 748 ms submit at 09:06:40 as the game first
  rendered in VR (both DLSS features were created in it, in 84 and 70 ms).
  18 of the 70 fall in the carrier scene: 31-92 ms each, and 159 ms at
  09:10:29.
- **What the split cannot clear.** Those two windows hold EDVR's draw hooks
  as well as Elite's own code. The draw-hook figure is sampled one frame in
  16 (`kDrawSampleEvery`), each sampled frame estimated from every 64th draw
  scaled by 64 (`kPerfMonitorDrawTimeStride`), and none of the seven LONG
  FRAME lines in the carrier scene fell on a sampled frame. The 1800-frame
  windows there read 0.87-1.45 ms per sampled frame, with maxima of 12.5 ms
  (window ending 09:08:09) and 14.6 ms (ending 09:09:13); one 0.2 ms call in
  the stride reads as 13 ms, so the maxima neither clear the hooks nor
  convict them. One candidate: `engine_velocity.cpp`'s `patchedVsFor`,
  `patchedPsFor` and `guardedOverlayPsFor` patch a shader and create it
  inside Elite's draw, on the first draw of each new shader in a family,
  which a newly arrived ship would trigger.
- **The steady state at the carrier.** Frame-cycle windows 7-9
  (09:09:29-09:10:29), p50s: the wait 0.09 ms (no slack; 78.5-84.1 fps);
  poses to second submit returned 6.6-7.1 ms; the real DXGI Present 0.056 ms
  and EDVR's hook on it 0.057 ms (`native_post_submit_phase`); then 4.4-4.8
  ms after the Present before Elite's next WaitGetPoses; cycle 11.6-12.4 ms.
  The GPU (census at 09:09:39 and 09:10:09): application render p50 12.18
  and 11.29 ms, EDVR 5.35 and 5.20, the game about 6.8 and 6.1. EDVR's part,
  same order: upscaler 2.89 and 2.87 (two a frame); UI layer reissues 0.59
  and 0.36 (107 and 93 a frame); hologram passes 0.39 and 0.44; UI resolve
  0.32 and 0.33; hologram resolve and celestial 0.29 and 0.27; UI layer
  composite 0.26 and 0.27; engine velocity 0.22 and 0.26; UI depth coverage
  0.18 and 0.23; motion prep 0.15 and 0.16. Both halves sit at the 11.1 ms
  budget. In window 6 (09:08:59), before the scene filled, the wait still
  had 2.6 ms of slack at 88.8 fps, with 5.2 ms pre-submit and 2.8 ms after
  the Present, while the application render read 11.0 ms.
- **fpsVR, 0.16.2 against this build.** Under OpenVR, fpsVR's CPU frame time
  is poses ready to second submit plus the compositor's submit cost; 0.16.2
  copies it as `appCpuMs` (`src\common\frame_flag.h` at the tag). The same
  window here is 6.1-7.1 ms p50 at the carrier (windows 5-9), against
  5.4-6.2 on 0.16.2. fpsVR's 10.8 ms on this build matches Elite's whole
  frame outside the wait (11.6 ms) instead, so SteamVR evidently times an
  OpenXR application over a longer window, one that takes in the 4.4-5.3 ms
  after the Present. Like for like this build reads 0.5-1 ms higher, about
  EDVR's sampled draw-hook cost; SteamOS runs Elite through DXVK, so that
  gap is not cleanly EDVR's.
- ruled out: EDVR's Present hook as a cause of the long frames, because none
  of the 70 long cycles in flight 090608 carries 2 ms of it (7.4 ms across
  all 70); in the steady state it is 0.057 ms p50.
- ruled out: GPU backpressure in Elite's Present as the carrier's
  post-submit time, because the real Present is 0.056 ms p50 (p95 0.10)
  there; the time comes after the Present, in Elite's code and the EDVR
  hooks its calls reach.
- ruled out: this build doubling Elite's CPU frame against 0.16.2, because
  the two fpsVR figures time different windows; the like-for-like window is
  6.1-7.1 ms against 5.4-6.2.

**Next.** In order of cost:
1. Which side limits the frame. HMD Image Quality is already 0.5 in these
   flights, the bottom of the normal DLSS range (below it the served-floor
   path changes the output too), so there is no lower-resolution test. The
   log cannot separate it either: when the scene thinned at 09:08:59,
   Elite's thread fell from 11.4 to 8.1 ms and the GPU from about 11.1-12.3
   to 10.6-10.7 ms together. Measure it instead: the GPU's idle time between
   one frame's last EDVR pass and the next frame's first producer run, in
   the census. Near zero means the GPU is the limit and the thread's excess
   is waiting on it; a gap means the thread starves the GPU by that much. A
   free alternative: EDVR's anti-aliasing off for a minute at the carrier
   (live), reading the time after Elite's Present; it also trims some of
   EDVR's hook work, so it is not clean.
2. An exact per-frame figure for EDVR's draw hooks, split into the
   pre-submit and after-Present windows and named on long frames: clock only
   the branches that do EDVR work, as `frame_ticks.h` does for the Present
   hook, plus the Create* hooks and the lazy engine-velocity patch. One
   carrier flight then says how much of Elite's 11.6 ms, and of the 31-92 ms
   frames, is EDVR's.
3. GPU: the output-resolution UI and hologram passes (1.9-2.0 ms) cut to
   where the UI is, and the reissues batched (architecture review P3), with
   no change to the image. CPU: the lean per-draw thunk (review A-1) against
   the 0.9-1.45 ms of draw hooks.
4. Settings: the output size. 4032x3896 per eye drives the upscaler, those
   passes and, through performance mode's half per axis, the game's render.

## 2026-09-29: flights 094126 and 094331, anti-aliasing off at the carrier

Same build (v0.18.0-rc.3-108-gc1408551) and install, HMD Image Quality 0.5,
with `fix.temporal_aa` switched live from the menu. 094126: DLSS, then TAA
at 09:41:57 (Elite's render grew to 4032x3896), off at 09:42:31, DLSS again
at 09:43:08. 094331: DLSS, then off at 09:44:16 (the render stayed
2016x1948) to the end at 09:45:44; its busy stretch is the last half
minute. Sean, reading fpsVR with AA off in a similar carrier scene: GPU 2.4
ms, CPU 4-5 ms. The mip bias (-1, `advanced.texture_lod_bias = auto`) is
set at launch from the launch mode, so every leg had the same one.

- **With AA off the carrier has headroom on both sides.** 094331's busy
  stretch: pre-submit (benchmark CPU) 3.2-3.5 ms p50, application render
  5.1-6.0 ms p50 (benchmark, 09:45:26-09:45:40); its last full window
  (09:44:51-09:45:21) read 88.4 fps with 5.6 ms of slack in the wait.
  fpsVR's 4-5 ms CPU again matches Elite's frame outside the wait; its 2.4
  ms GPU is below EDVR's own 5-6 ms, and what fpsVR times on the native
  runtime is not established.
- **The census misses part of the AA cost.** 094126's TAA leg against its
  off leg, adjacent minutes at the same 4032x3896 render: application render
  10.95-11.12 against 7.13-7.47 ms (benchmark), about 3.7 ms, of which the
  census itemised 1.96 (TAA window, 09:42:27). About 1.7 ms landed inside
  Elite's own draws, where the census counts it as the game's. Candidates:
  the pool-family draws that record EDVR's slot and depth, and Elite's UI
  draws that EDVR takes into its output-size UI layer.
- **Engine motion's CPU is untimed, and the carrier is its worst case.** The
  emit hook (FUN_144312E00) ran 1,801-2,930 calls a frame, with 431-1,152
  records joined a frame, at the carrier in flight 090608
  (09:08:08-09:10:08), against 127 and 105 in 094126's TAA leg; with AA off
  it does not run. No timer covers it or the copy observers: the draw-hook
  figure is EDVR's time in Direct3D draw calls, and these relays run inside
  Elite's own code. At 1-1.5 us a call they would be 2-4 ms of Elite's
  thread at the carrier.
- So the carrier's DLSS frame is either GPU-bound (EDVR's GPU work past the
  budget, Elite's thread waiting inside its own calls) or bound by EDVR's
  untimed CPU work (the GPU waiting on the thread). Switching AA off removes
  both at once, and engine motion has no switch of its own (it arms with
  the mode, `temporal_pass.cpp`).
- ruled out: EDVR's anti-aliasing work as the cause of the long stalls,
  because 094331 logged 160 and 138 ms stalls (09:44:56, 09:45:40) with
  `fix.temporal_aa` off, while EDVR's draw hooks read 0.02-0.36 ms a sampled
  frame; the stalls are Elite's. The 441 and 191 ms at 09:44:16-20 and the
  1.2 s at 09:42:07 are the mode switches.

**Next.** One instrument build, then one DLSS flight at the carrier with
many ships about:
1. Engine motion's CPU per frame: the emit relay, the copy observers
   (clear, merge, copier) and the pool-family draw recording, timed on the
   thread that runs them and summed per frame, in a 30 s line and the LONG
   FRAME line's EDVR share.
2. The GPU's idle time between frames, in the census: near zero means the
   GPU is the limit; a gap means the thread is.
3. Census sections for the Elite draws EDVR alters: the pool-family draws
   with EDVR's slot bound, and Elite's UI draws in the UI layer.
Then the fix follows the figures: the join's per-call cost, the UI passes
(review P3), or the output size.

## 2026-09-29: the carrier instruments, built, not flown

Sean approved the instrument build the 094331 entry asked for. Branch
`claude/carrier-instrument` (from main c4bbe484): three always-on instruments,
no config key, each gated by a rig in `build.bat`. Nothing here has flown.
They answer one question at the busy carrier with DLSS on: is EDVR's GPU work
the limit (H-GPU, Elite's thread waits inside its own calls) or is EDVR's
untimed engine-motion CPU work the limit (H-CPU, the GPU waits for the
thread)? The environment they will be read in: SteamVR OpenXR, Pimax, 4032x3896
out and 2016x1948 in per eye, 90 Hz, DLSS preset K, `fix.temporal_aa` on
(engine motion arms with it), `advanced.app_gpu_timing` on (its default; the
gap needs it).

**1. Engine motion's CPU, per frame, every call clocked.** EDVR's own work in
fifteen relays into Elite's code and in the draw side, never the game function
a relay forwards to. A relay is bracketed enter, pause, (game code), resume,
leave, so the forward is out and a relay the game's code calls is its own part.
A scope inside EDVR's own work (the shader patch inside the draw side) is
taken from its parent, so the parts add up with nothing counted twice. Each
thread writes its own counters (relaxed load, add, store: no lock prefix, no
shared line); the Present hook cuts the frame, and the thread that cuts is the
render thread. The parts: emit (FUN_144312E00's bracket and the emit
observer), rigid emit (FUN_1442B4130), copier, merge, clear, jobs (the six job
brackets), builder (the draw-item builder, the second direct producer, the part
test, the LOD setter), draw side (the slow half of `engineVelocityBeforeDraw`,
lock wait included), apply (`primaryCopy::apply`, inside the draw side), tees
(Map, Unmap and write tees on watched resources), shader patch (the three lazy
patches, on a cache miss only). Three lines every 30 s:

```
engine motion CPU: 30 s, 2700 frames; ... Render thread 4321: total p50 0.31 / p95 0.52 / max 1.94 ms per frame over 68.0 clocked calls per frame. Other threads: 1310.00 ms per s across 7 threads over 2647.0 clocked calls per frame. Clock floor: a timed scope records 12 ns and costs 33 ns (32 and 66 ns with a forward pause; the fastest of 4 batches of 512 null pairs, measured as this window closed), so these figures include about 0.086 ms per frame of floor and the instrument costs about 0.177 ms per frame.
engine motion CPU, render thread, ms per frame p50/p95/max (calls per frame; longest call ms), "-" = the code never ran on this thread in the window: draw side 0.20/0.35/1.20 (52.0; 0.21), apply 0.08/0.12/0.60 (2.0; 0.50), tees 0.01/0.02/0.05 (14.0; 0.02), shader patch 0.00/0.00/0.00 (0.001; 2.10), emit -, rigid emit -, ...; shader patches this window on every thread: 3, total 4.20 ms, longest 2.10 ms.
engine motion CPU, other threads (7), ms per s (calls per frame; longest call ms), "-" = the code never ran off the render thread in the window: draw side -, ..., emit 1200.00 (2450.0; 0.04), rigid emit 30.00 (127.0; 0.01), copier 70.00 (40.0; 0.09), ..., eval 5400.0 calls per frame, not clocked (a pass-through).
```

(Numbers are the rig's sample, not a flight.) The LONG FRAME line's EDVR share
gains ` engine motion 0.31 ms;` at its end: this frame's own render-thread
figure, exact, or ` engine motion none this frame;` when no hook ran on the
render thread. It is inside "outside the hook", like the draw hooks, and the
draw side is inside the draw-hook estimate too: do not add the two. To keep the
line under the log's limit the draw-hook freshness wording is now "(held over)"
(its worst case was 1144 characters; with the clause it is 1154 against a gate
of 1160 and a real limit of 1166).

What is not clocked: the two evaluator relays are pass-throughs (two loads and
a call) cheaper than one clock read, so every call is counted and only the
probe branches are clocked; the line says "not clocked". The quick path of
`beforeDrawSlow` and the inline half of `engineVelocityBeforeDraw` (compares,
tens of thousands of draws a frame) are neither clocked nor counted; the
frame-boundary work (`engineVelocityFrameBoundary`, the views, the summary
line) is in the tick chain's "slowest EDVR ticks". The flat profile never
reaches the monitor tick (it has no LONG FRAME line either), so none of the
`engine motion CPU` lines print there, and its draws go through their own
scope, so section 3 does not count them.

The instrument's own price is calibrated on the real clock as each window
closes, the way the census states its timer floor: null scopes on the render
thread in the CPU state the window ran in (a clock that idled at the menu is
not the flight's), both shapes, four batches each with the fastest kept (a
batch a preemption inflated is not the floor), what a null scope records
(every figure includes it once per call) and what it costs.
On the build machine (QPC 10 MHz, a reading about 16 ns) a plain scope records
12-19 ns and costs 33-35 ns; one with a forward pause records 32-33 ns and
costs 66 ns (the rig prints it on every build). At the carrier's 2,930 emit calls a frame that is about 0.19 ms of job-
thread time a frame; the render thread's draw side is 50-ish calls a frame,
under 2 us. Each window prints its own figure (the last sentence of line 1); the
calibration itself is about 0.2 ms once a window, on the render thread.

**2. The GPU's gap between frames.** The Application-render span already
brackets a frame's producer GPU work on the game's device: its first
timestamp at the first producer command after the pose wait, its last at the
end of the final segment, after the door work. The result now carries those two
raw ticks and the clock's frequency. `gpu_frame_gap.h` pairs frame N's last
tick with frame N+1's first by consecutive sequence number. The census's clock
rules hold: validated spans only, one frequency, no negative gap, no gap over
1 s; results complete out of order, so each pair is made once by whichever
frame arrives second; a frame with no valid span leaves its two pairs unmade
(fewer pairs than frames, never bridged). No new GPU work: it reads timestamps
that already exist. On the main census line, after "application render p50":
`frame gap p50 0.42 / p95 1.85 ms over 2650 pairs;` (`frame gap - (no pairs);`
when nothing paired). A line of its own follows with the max, the frame count,
rejected pairs, and the caveat. **The gap is an upper bound on GPU idle, not
idle.** SteamVR's compositor is another process on the same GPU and takes its
turns in that gap; so do the runtime's transfers and the mirror window's
Present copy. A gap near 1 ms is therefore not proof of idleness. A gap near
zero is the strong statement: nothing waited.

**3. Sections for the Elite draws EDVR alters.** EDVR's own commands were
timed; the cost it adds inside Elite's draws was counted as the game's. Four
classes, from reading `forwardWithVerdict`, each a real per-frame count, one
class per draw in this priority: pool-family draws with EDVR's MRT6 slot
target bound and its shaders substituted; terrain prepasses with EDVR's motion
target bound and a pixel shader added (celestial motion); UI draws redirected
into EDVR's layer (`fix.ui_quality`); draws wrapped in another fix's state
change (RemLok, holo, scrim, particles, the panel). Ordinary per-draw census
sections: K = 8, the existing stride and rotation (21 sections now, so each
is timed one frame in 21), the null-pair correction, their spans in the timer
floor and the spans count. Each thunk wraps only its real draw call, so the
weapon and screen motion reissues after it, and every other issue through the
same lambda, are never timed as altered. A line after the main one:

```
EDVR GPU census, Elite's own draws that EDVR alters (each is the game's draw timed whole, so a figure includes the game's own work in it, not only what EDVR adds, and none of it is in EDVR ~X above): pool-family draws (EDVR's slot target and shaders) 2.000 (10.00/frame), terrain prepasses (EDVR's motion target and shader) -, UI draws (redirected to EDVR's layer) 0.020 (0.20/frame), other fix-wrapped draws -; together 2.020 ms/frame; "-" means no such draw ran this window.
```

These are the game's draws timed whole: the figure includes the game's own work
in them. It is not EDVR's cost and is never in "EDVR ~X". The AA cost inside
those draws is what shows when the same scene is flown with AA off and the
figure drops.

**Flight procedure.** Install the build on Frontier and check the log names it
(`python tools\edvr_log.py --target frontier --expect-build HEAD --version`,
exit 0). Same settings as flights 094126 and 094331 (HMD Image Quality 0.5,
DLSS on). Fly to the busy fleet carrier with many ships about and stay steady
for at least two minutes: four full 30 s windows, of which the first carries
the warm-up (the first shader patches, the calibration). Then, in the same
scene, switch `fix.temporal_aa` off from the menu for a minute: with AA off
every engine-motion part must read "-" (the relays are closed; a job bracket or
the evaluator can still run for another consumer of the eval gate, the static
prop gate or the LOD governor), and the gap and the altered draws give the
baseline. Read with
`python tools\edvr_log.py --target frontier --grep "engine motion CPU|EDVR GPU census|LONG FRAME"`.

**Reading H-GPU against H-CPU.** The thresholds are rules of thumb from the
numbers above (a 11.1 ms period, about 1 ms the compositor and transfers can
hold), not measurements; the flight sets them.

| Line | H-GPU | H-CPU |
|---|---|---|
| census `frame gap` | p50 near 0 (under about 0.3 ms), application render p50 near the 11.1 ms period: the GPU never waited | p50 above the roughly 1 ms the compositor and transfers can hold, p95 several ms, application render well under the period |
| `engine motion CPU`, render thread | total p50 under about 0.5 ms | total p50 1 ms or more; the parts say where (draw side, apply, tees) |
| `engine motion CPU`, other threads | ms per s small against the job threads' capacity | hundreds of ms per s in emit, rigid and copier, and the render thread's pre-submit wait absorbing them |
| LONG FRAME `engine motion X ms` | small on the long frames | large on the long frames |
| altered draws | the AA leg's figure (the earlier estimate: 1.7 ms) is large and disappears with AA off | small |

A render-thread draw side or apply with a small p50 and a large p95 or max is
a stall inside the slow half (the engine mutex or primaryCopy's mutex behind a
job thread, or the driver's queue), not steady work. If the gap is near zero,
the render thread's engine-motion total is small and the altered draws are
small, the remainder is Elite's own work at the doubled render size, and the
output size is the lever. If the gap is large and engine motion is small, the
wait is Elite's (a pose or job wait) and EDVR is not the limit.

**What appears in the log if the code never ran.** Each figure has its own
sign. The CPU parts print "-" and the summary reads "over 0.0 clocked calls per
frame"; the LONG FRAME clause reads "none this frame". The gap prints "frame
gap - (no pairs)" and the detail line says why. An altered class prints "-".
No `engine motion CPU` lines at all means the recorder never ran (the Present
hook's monitor tick did not), and a LONG FRAME line with no engine-motion
clause is the priming frame or a build without this instrument.

**Rigs.** `engine_motion_cpu_test` (new): exclusive nesting and the forward
pause, thread attribution, no lost update under four writers and a cutter
(400,000 calls exact), percentiles in a hostile order, "-" against 0.00, the
three lines at their worst (693, 973 and 659 of 1150 allowed), the priming
frame, the 30 s and full-window closes, per-call maxima per window, the clock
floor exact on a clock that steps (and unmoved by a simulated preemption) and
measured on the real one; mutation-checked (eleven mutants, all caught).
`native_perf_history_test`: the LONG FRAME clause and its worst case.
`gpu_census_test`: the gap saturated, starved, compositor-sized, with a missing
and a late pair, a duplicate, another clock, an overlap, a stall, and every
census line at its worst (914 of 1150); the class table and its priorities, the
altered scope's counting and timing, the rotation over 21 sections, the line.
`gpu_span_state_test`: the tick fields for three and four segments and none for
a bad or legacy span.

**Found in passing, not changed.** `beforeDrawSlow` reads the clock at entry on
every visit, including the quick path that returns a few compares later; the
reading is only used by the slow path. At ten thousand visits a frame that is
about 0.16 ms of the render thread, and moving the line below the quick-path
return changes no figure. `primaryBuildObserved` does a relaxed `fetch_add` on a
shared counter per call from job threads; the new "rigid emit" figure will show
what that costs.

## 2026-09-29: flight 112704, the carrier instruments flown: GPU-bound

Build v0.18.0-rc.3-158-gcd37e487 on the Frontier install (build matched),
SteamVR OpenXR (`aapvr`), 4032x3896 out and 2016x1948 in per eye, HMD Image
Quality 0.5, DLSS performance, preset K, launched in DLSS. Logs
`edvr_gfx_20260929_112704.log` and
`edvr_openxr_20260929_112705_668_46812.log`. At the fleet carrier with many
ships from about 11:29 (emit 2,310-2,968 calls a frame, as busy as 090608),
then anti-aliasing off at 11:31:36 in the same place.

- **The GPU is the limit.** Census windows ending 11:30:04-11:31:34, DLSS:
  frame gap p50 0.06-0.10 ms, p95 1.38-1.61 ms (2,297 of 2,297 frames paired
  in the busiest); after AA off, 4.51-4.63 ms p50. With DLSS the GPU never
  waits for the next frame's work; without it, it waits about 4.5 ms.
  Frame-cycle windows w6-w9: cycle 12.67-13.15 ms, 75.7-77.8 fps, the wait
  0.09 ms; after AA off (w10-w11) 11.12-11.23 ms with 4.4-4.5 ms of slack,
  87-89 fps.
- **Engine motion's CPU is small on Elite's thread.** Render thread p50
  0.35-0.40 ms a frame (p95 1.10-1.22, max 1.65-2.85) over about 1,600
  clocked calls; the largest parts are the clear observer (0.21 p50, 0.57
  p95) and merge (0.49 p95). Other threads: 241-273 ms per second across 7
  threads, about 3.5 ms of job-thread time a frame: clear 144.6 ms/s (702
  calls a frame, about 2.7 us a call), builder 56.8 (13,425 calls a frame),
  merge 45.8, emit 23.2. The clock itself costs about 1.17 ms a frame, mostly
  on those job threads.
- **Where the GPU frame goes, busiest window (12.726 ms p50).** EDVR ~5.732:
  upscaler 3.078; UI resolve 0.332, UI layer composite 0.277, UI layer
  reissues 0.428 (114 a frame), UI depth coverage 0.212; hologram passes
  0.691 (5.5 a frame), hologram resolve and celestial 0.218; engine velocity
  0.299, motion prep 0.157. Game ~6.99. Against the AA-off windows
  (application render 6.46-6.56 ms) the AA path costs 6.0-6.2 ms, so about
  0.3-0.5 ms of it lies inside Elite's own draws, against the TAA leg's 1.7
  at twice the render size. Elite's draws that EDVR alters, timed whole:
  pool-family 0.183 (2,314 a frame), UI draws redirected to EDVR's layer
  0.600 (112 a frame), other fix-wrapped draws 1.021 (6 a frame).
- The long frames in the busy windows (22-47 ms) carry engine motion
  0.33-0.78 ms and draw hooks about 1 ms; the rest is Elite's, as before.
- ruled out: EDVR's engine-motion CPU as the carrier's limit, because it
  costs Elite's render thread 0.35-0.40 ms p50 a frame while the GPU never
  waits between frames (gap p50 0.06-0.10 ms); the thread's extra time with
  DLSS is mostly waiting on the GPU.

**Next.** The GPU is the lever: EDVR needs about 1.6 ms off its ~6 ms at the
carrier for the p50 to reach 90 Hz. In rough order of gain:
1. The upscaler, 3.08 ms: its cost follows the output pixels, a setting;
   foveated DLSS is built and paused by Sean.
2. The UI and hologram passes, 2.16 ms: restrict the resolve, composite and
   hologram passes to where the UI and holograms are (review P3), and look
   at drawing the redirected UI and the reissues at the render size.
3. The six "other fix-wrapped draws", 1.02 ms whole: name them and find how
   much of it is EDVR's.
Separately: sample the engine-motion clock (every call now, ~1.17 ms of CPU a
frame) now that it has answered; the clear observer (2.7 us a call on job
threads) is the CPU item for scenes that are CPU-bound.

## 2026-09-29: the engine-motion clock sampled, the fix-wrapped draws named (built, not flown)

Branch `claude/engine-clock-sampling`, on main 446d7e7a; both changes are the
two Sean approved after flight 112704.

**The clock costs about 0.12 ms a frame, not 1.17.** The flown instrument
clocked every hook call on every thread: 17,499 a frame on the job threads
(13,425 the builder) and about 1,600 on the render thread. Now
(`engine_motion_cpu.h`):
- The render thread (whoever calls Present) is clocked on every call of every
  frame, so its figures and the LONG FRAME "engine motion" clause are exact and
  unchanged.
- Every other thread is clocked only on sampled frames, one in 32 at a random
  place in each block of 32 (a fixed stride would alias with anything Elite
  does every N frames). On the other 31 a hook is a load of one gate word, a
  thread-id compare (from the TEB, no call) and a branch: no clock read, no
  slot, no count. A scope decides once, at entry. The evaluator relays (counted,
  never clocked) count on sampled frames only.
- The other threads' figures are per SAMPLED frame (thread-ms mean, p50, p95,
  max; calls per sampled frame); the window totals keep a scope that straddled a
  cut. If the thread-id read ever disagreed with `GetCurrentThreadId` the
  scheme drops to clocking everything and the summary says "Sampling is OFF".
- Four 30 s lines: the scheme and totals; the clock and what the instrument
  costs at this window's rates weighted by the sampled fraction; the render
  thread's parts; the other threads' parts. The job bracket in
  `kinematic_eval_hook.cpp` takes its own two readings and shares them
  (`pauseAt`/`resumeAt`), so the L1 job statistics do not depend on sampling.
- What a null scope RECORDS is now measured with a random wait between the
  scopes, median batch: back to back, three 33 ns scopes make one 100 ns QPC
  tick and one rig run read 0.2 ns where the next read 12-16, the same code.

Cost, rig on this machine: a clocked scope costs 33.4 ns and records 15-16 (66
and 32 with a forward pause); a skipped scope 0.6 ns, a counted call 0.4. At
the 11:30:34 window's rates (render thread 1,045 plain and 557 paused calls a
frame; job threads 702 plain and 16,797 paused; 14,850 evaluator counts) the
model gives 1.21 ms for the flown scheme (its own line said 1.172) and 0.12 for
one in 32 (render thread 0.072, the price of keeping it exact; others 0.05).
One in 16, the first suggestion, gives 0.155, over the 0.15 target; one in 32
holds 0.140 even with a skipped scope priced at 1.5 ns, 2.6 times the measured.
The floor is a hot loop: the flight's own clock line, computed the same way
from that window's rates, is the check.

**The census names the fix-wrapped draws.** "Other fix-wrapped draws" is now
one section per fix, 19 (the 18 verdicts that reach the altered-draw site, and
an "unnamed" row that shows if a wrapped draw's fix has no name). They share one
turn (the rotation stays 21 long, so nothing else is sampled less), one K of 8,
one stride from all their calls together and one empty timer pair a turn. The
classes' line is unchanged, its "other fix-wrapped draws" now their sum (1.021
compares directly); a line after it gives each fix's ms and calls a frame, "-"
where none ran (937 characters worst case). `vscreen.cpp` maps `DrawVerdict` to
fix in a switch with C4062 an error: a verdict added without a name does not
compile. Nothing renders differently.

**Which code wraps the six: not established.** No log counts draws by verdict.
What flight 112704 says (windows 11:27:34-11:32:34): 2.00-2.83 a frame in menus,
4.96, 7.76, 8.03 as ships gathered, then exactly 6.00 for three windows (0.92-1.02
ms with DLSS) and still 6.00 with AA off at 2.416 ms (0.40 ms a draw against
0.17). The cost per draw rose 0.07 to 0.17 ms as the count fell: large
translucent draws, fill-bound. The UI layer's line in the same window reads
"left in the game's frame: hologram 4.00 a frame (another fix swallows or
re-issues it)": four radar-family draws a frame (icon core, stalks, contact
markers) arrive under a verdict that does not forward, so they are in this class.
- ruled out: the six as the holograms redirected into EDVR's layer, because a
  redirected draw is class UiLayer (tested before Verdict in
  `classifyAlteredDraw`) and that is the 0.600 ms row; and as the hologram depth
  reissues (5.5 a frame, 0.691 ms), because those are EDVR's own draws with their
  own section, not the game's.
- Candidates, each with the row that shows it: the sun glare train (`sun glare
  steady`; the log has VIVID and the world variant created at 11:27:29; matcher
  kind N, 6 indices, 2+ instances, 2048x1024 BC7 in PS slots 0 and 1); the radar's
  contact markers caught by that same shape (instanced quads, one instance a
  contact: also `sun glare steady`, the four hologram draws inside the six);
  particles; night vision; the HUD and holo verdicts. `unnamed fix` above zero is
  a verdict with no name. If `sun glare steady` reads 6.00, one live flip of
  `fix.sun_glare` to stock in the same place separates the two: the "hologram ...
  left" line vanishing with the redirected holograms up by 4.00 says the glare
  matcher is catching the radar, and the row's cost stock against vivid is what
  EDVR's substitution adds.

Tests: `engine_motion_cpu_test` 144 checks (sampled frames exact; unsampled
frames read no clock, take no slot, count nothing; gate, schedule, fold over
sampled frames, a straddling scope, the fallback, the cost model reproducing
1.17, the four lines' worst case 991 characters), 44 mutants, all caught;
`gpu_census_test` 134 checks, 21 mutants, all caught.

What the next flight's log must show: the summary reads "Other threads (N) are
clocked only on the M sampled frames (one in 32" with M about 3% of the frames
and the clock line "the instrument costs about 0.12 ms"; if sampling had not
run it would say every frame, or "Sampling is OFF". The census line "EDVR GPU
census, the other fix-wrapped draws above by the fix that wraps each" sums to the
classes' "other fix-wrapped draws"; every fix "-" beside 6.00 there means the
split is dead.

## 2026-09-29: the UI and hologram passes, built, not flown

Branch `claude/ui-holo-passes` from 446d7e7a. Asked for: the UI and hologram
GPU passes cut to the work they have, the image unchanged (review P3, B-3,
D-1, D-2, D-4). Nothing merged, installed or flown. Commits 396d8dcd (the rig
and its goldens, recorded from the unmodified shader first), f282c056 (the
change), 582193da (the composite bench, this entry) and 8ced6ab5 (the totals
line). Every number below is an RTX 5090 at the eye's real size, 2016x1948
in and 4032x3896 out, from `tools\ui_holo_pass_test` (`--bench`,
`--bench-composite`, `--adapter nvidia`): the median of 25 interleaved rounds
of 8 dispatches between GPU timestamps, inputs synthetic (a mixed-content
frame, a HUD's panels over 15% of the eye).

**Built: the UI resolve does not fetch what it was not given.** A load from a
null view returns zero and is slow on this GPU. Reference (the shader as it
was) against production, ms an eye:

| state of the inputs | reference | production |
|---|---|---|
| everything bound, idle / HUD / menu | 0.145 / 0.143 / 0.165 | 0.144 / 0.142 / 0.170 |
| source-edit mask unbound, idle / HUD | 0.179 / 0.175 | 0.135 / 0.131 |
| coverage mask unbound, idle | 0.179 | 0.134 |
| history unbound, idle | 0.155 | 0.125 |
| all three unbound (no UI this frame) | 0.245 | 0.109 |

Flight 112704's census read the resolve at 0.55-0.58 ms a frame in its first
minute (windows ending 11:28:04 and 11:28:34) and 0.33-0.41 after, 0.33-0.37
through the busiest; the reference's all-unbound and edit-mask-unbound rows
doubled are 0.49 and 0.36. That is inference, not a measurement: the log does
not say which inputs were bound. What supports it: the UI content tracker
read `compared=0` until 11:28:51, and `g_edits[eye].marked` is set only by a
surface composite whose source changed, so a still HUD leaves the edit mask
null on most frames. Expected: about 0.09 ms a frame in the busy windows
(0.357 to about 0.27), up to 0.3 in the first minute's state, nothing when
every input is bound (parity, 0.144 against 0.145). To read it after a flight:
`UI resolve totals: N dispatches this session; without the coverage mask a,
without the source-edit mask b, without history c` (every 20 s while it
moves) says how often each input was null, which settles the inference; the
census's UI resolve in the same scene says what it bought.

How: b1.z carries the unbound inputs (bits 1 coverage, 2 source edits, 4
history; zero is all bound, what a caller with no b1 sends), derived in
`applyUiResolve` from the views it binds. The shader body is compiled twice,
once with the bits known zero (the checks fold away) and once asking. Same
arithmetic, same history layout. Proof, gated in build.bat as
`:rig_ui_holo_pass_test`: 269 fixtures byte for byte against a frozen copy of
the shader and against goldens recorded before the change (WARP; the RTX
passes the pairs too): the corner, tile edges, the corona's edge, none, a
full-screen menu, jitter to 1.7 and wild, ratios 0.5 to 3, both eyes; each
input unbound alone equals the reference over zeros (227 runs); bits of zero
over unbound inputs still equal the reference (98); claiming a bound input
unbound FAILS 203 of 219 fixtures with content, and each bit fails on the
named fixture built to need it. Mutation: the marks' halo cut one texel short
(the resolve's own margin around UI) fails 359 checks, at the corner, the tile
edges and the goldens. Trace: a build without this has neither the
totals line nor `UI resolve: not bound on some frames`, a run with every input
bound shows totals with zeros, and the bytecode header is checked against the
source, so a stale header fails the rig.

- ruled out: an 8x8 tile early-out for the UI resolve, because there is no
  corona radius to key a margin on. The corona hold applies to any faint flat
  pixel anywhere (`corona_smear_level` 64 by default), so every tile needs
  its raw neighbourhood, and the pass is bandwidth-bound anyway: with the hold
  off it takes 0.112 ms, 188 MB at 1.7 TB/s, against 0.076 ms for a bare
  compute copy of the frame.
- ruled out: a groupshared window for the resolve's taps, because it is
  slower with the inputs bound. Built and exact (269 fixtures, both adapters);
  idle 0.141 to 0.149, HUD 0.139 to 0.150, menu 0.156 to 0.205 (0.129, 0.129,
  0.178 with the fallback taken out): the taps are L1 hits and the pass sits
  at its bandwidth floor. It only won where inputs were null, which the
  b1 bits do without it.
- ruled out: an R8 history for the resolve, because it buys 2-3% (0.140
  against 0.144 idle, 0.139 against 0.142 HUD; about 0.007 ms a frame) and
  the pair is shared with the own path's RGBA colour evidence.
- ruled out: bounding the layer composite to the layer's tiles, because it
  costs 0.119 ms with an EMPTY layer, 0.120 with a HUD's panels and 0.130
  covered, against 0.076 for a bare copy of the frame: a perfect skip of
  empty tiles saves 0.04 ms an eye at most (about 0.07-0.09 a frame), and
  nothing exact says which tiles are empty (every redirected draw uses the
  full-eye viewport; the draws' bounds live in the game's own vertex data).
  The flight agrees: the layer's own composite price is 0.118-0.120 ms an
  eye-frame in every window from 4 redirected draws a frame to 122. What a
  skip would rely on holds: over an empty layer the composite returns the
  frame byte for byte.

Not changed, and why:
- **Hologram passes and their scratch (D-4).** The scratch is cleared lazily,
  once an eye-frame at the first listed draw (`holoScratchPrepare`); the
  near-light pass and the resolve already discard on an empty element depth
  and read the game's target and display only under flags. What costs (0.126
  ms a call, 5.5 a frame) is each listed draw's two replays of the game's own
  geometry, and a scissor to bounds needs bounds nothing on the CPU has.
- **One scene-depth copy (D-2).** `ui_depth_layer.h:89` copies at the first UI
  draw of an eye-frame; the layer's seeds copy the depth and stencil as they
  stand at each seed, and the stencil is rewritten between them by the
  stencil-only writers the seed arc counts (docs/dlss-performance-review-
  2026-09-28.md, ruled out: keeping stale depth). Sharing changes pixels.
- **The reissues (114 a frame, 0.428 ms).** About 4 us each: a small draw and
  a render-target switch. There is no bound to take; fewer needs merging draws.
- **"Other fix-wrapped draws" (1.02 ms, 6 a frame)** are Elite's own draws
  under another fix's state (`AlteredVerdict`, timed as the game's draw and
  only it), not EDVR's hologram replays: those are the "hologram passes" bucket.

**Largest left, outside this brief.** The layer's HDR HUD depth-stencil seed,
in no census section: in flight 112704's layer lines it is 0.52-0.53 ms an
eye-frame median (p95 1.2-1.4) in every window from 11:29:34 to 11:31:34,
1.04 ms a frame, with 80-122 redirected draws a frame, three seeds and one
stale per eye-frame (13,782-14,520 seeds, 4,594-4,840 stale a window); at 54
draws a frame (11:29:04) it is 0.158, and 0.156 in the quiet station flight
of 09-28. The exact routes to fewer seeds are ruled out in
docs/dlss-performance-review-2026-09-28.md. Second, `fix.ui_quality` 1.25
itself: the layer holds 1.62x the pixels of 1.0, and the seed, the
redirected draws (0.60 ms) and the composite scale with them, roughly 0.7-1.0
ms a frame by pixel count, unmeasured. That changes the image; Sean's call.

## 2026-09-29: flight 132352, the carrier again, the six draws named

Build a43c94ba on the Frontier install (build matched). Launched with AA off
(the previous flight's live switch had written it to edvr.ini), so the mip
bias was 0 all session; DLSS from 13:25:12; at the carrier from about 13:26.
Sun glare vivid -> stock at 13:27:28 and back at 13:28:13; UI quality
125 -> 100 at 13:28:29, then a trip through the main menu. F10 at 13:26:45
(the draw census and 117 eye dumps). Sean: the stock glare made frame times
worse; CPU frame time good; GPU still hitches; 90 not held.

- **The six wrapped draws, named.** Vivid: `sun glare steady` 0.30-0.46 ms
  (about 4 a frame) and `scanner-body resolve` 0.04-0.07 ms (2 a frame),
  both timed whole. Nothing large to cut there.
- **Stock glare costs more than vivid.** With stock (census window ending
  13:27:53): the redirected UI draws 2.675 ms (116 a frame) against
  0.52-0.84 ms with vivid, the census's "game" 8.5 against about 5.9, and
  the GPU p50 13.1-14.0 ms against about 11.3 before and 10.9-11.0 after.
  Hypothesis, under test: the stock glare's full-screen overlays are taken
  into the UI layer and drawn at output size times UI quality.
- **UI quality 100 against 125: not settled.** The census's timed UI items
  did not move (resolve 0.26-0.31, composite 0.26-0.30), the comparison
  crossed a reload, and the part that should shrink, the HUD depth seed
  (the layer's depth-stencil target is 5040x4870 at 125 here), is not in the
  census yet.
- **The end of the flight sat at the budget.** Vivid and UI 100, back at the
  carrier: runtime window 13:29:09-13:29:39 held 89.9 fps with no late frame
  while the scene refilled (benchmark GPU 8.8 -> 10.6 ms); the census window
  ending 13:29:53 read 10.82 ms with the frame gap back to 0.19 ms p50.
  EDVR 4.75-4.88 ms of it (upscaler 2.93-3.02).
- **The hitches.** Long frames at the carrier ran 24-41 ms with EDVR's hook
  0.04-0.28 ms and engine motion at most 0.57 ms in each: Elite's. The
  233.6 ms at 13:26:45 was the F10 capture and eye dump. Otherwise the GPU
  median sits at the budget, so ordinary variance crosses 11.1 ms.
- **UI resolve totals:** 20,494 dispatches, the source-edit mask absent in
  20,486 and the coverage mask in 15,192, so the null-fetch cut applies
  almost always. Engine motion on the render thread: 0.12-0.15 ms p50.
- ruled out: the six "other fix-wrapped draws" as a large EDVR cost, because
  they are the `sun glare steady` row (0.30-0.46 ms) and the scanner-body
  resolve (0.04-0.07 ms). corrected (the seed-census entry): that glare row
  was the landing-pad rings, which the glare fix claims by shape, not a sun.
- Sean's eye dump shows the landing-pad display, which Elite draws in the
  radar's place while docking, left out of the UI layer (the radar itself is
  handled correctly); under analysis with the stock-glare question and a
  census section for the depth seed.

## 2026-09-29: the HDR HUD depth-stencil seed in the census (built, not flown)

Branch `claude/ui-seed-census` from d1eac52b. Asked for: the layer's HDR HUD
depth-stencil seed as a census section of its own, sampled like the others, so
the next flight measures UI quality 100 against 125 in one place. Log only:
the seed, the image and every setting are unchanged. Commit 0a407415 (the
section, its rig, the call site) and this entry. Nothing merged, installed or
flown.

**What the log will show.** A ninth in-frame item, last, `HDR HUD depth-stencil
seed X (n/frame)`, in the in-frame total and in `EDVR ~`; and, only when a seed
ran, the line after the main one (figures here are the rig's, not a flight's):

    EDVR GPU census, the HDR HUD depth-stencil seed above (the copy of the
    game's depth-stencil, then the passes that write it into the HUD layer's
    own): 3.50 seeds a frame, 0.200 ms a seed (96 timed); target 5040x4870
    D32_FLOAT_S8X24_UINT (196.4 MB), seeded from the game's 3024x2922.

- Counted for every seed, timed on the section's turn (one frame in 22: up to
  8 seeds and one empty pair), so the cost is a counter a seed and, on that
  turn, at most nine timestamp pairs. `ms a seed` is the corrected mean, and
  `seeds a frame` is what to multiply it by.
- **UI 100 against 125:** the target is on the line (4032x3896, 125.7 MB at
  100; 5040x4870, 196.4 MB at 125). A window the quality was changed inside
  says `the target changed inside this window, so the figure mixes them` and
  names both, so read two clean windows; do not read the straddled one.
- **What shows if it never runs.** No such item at all: a build without this.
  `HDR HUD depth-stencil seed -` and no line: no seed ran (the layer is off,
  or no HUD draw tested the game's depth). `-` while the layer's own line shows
  `HDR HUD depth-stencil seed .../... (N)` with N above zero: the scope is not
  wired (the rig's scan of `ui_layer.cpp` fails the build on that).
  `the seeds reported no target`: the scope ran without its target.
- **Across builds.** From this build on `EDVR ~` carries the seed and `game ~`
  no longer does, by the same amount: 0.4-1.0 ms a frame at the carrier (the
  layer's per-eye-frame medians in flight 132352, 0.19 vivid to 0.50 stock at
  125, times two eyes). Take it out of a new census before comparing with an
  old one. Those medians are per eye-frame (1.75-3.0 seeds each here) and the
  scenes differ (0.39 at UI 100), so they settle nothing about 100 against 125.

Proof (`tools\gpu_census_test`, 134 checks to 180): the section, its item and
its place; the line, the total, the line's position, its three target states;
the never-ran control and the window after a window with seeds; the scope on a
null context (HDR counted, noted and timed on its turn, the 8-bit layer's seed
counted nowhere, another section's turn, the K cap) and on WARP with real
timers; worst-case line lengths (main line 967 characters, seed line 539,
under the 1150 the log keeps). A scan of `ui_layer.cpp` holds the call site
(one scope, before the copy, the HDR stage, the layer's own size and format)
with seven mutants of it. 27 mutants of the production sources, run on a
scratch copy, are all caught, each by the check written for it (one at compile
time: the section's place is asserted); the unmutated copy passes first.

Not changed: the 8-bit layer's own seed is not in the census (its route read
`-` in all 12 windows of 132352, so nothing is lost; it would be a section of
its own). The layer's route timers and the `HDR seed subprice` diagnostics are
untouched.

## 2026-09-29: flight 162819, the landing-pad rule reverted

Build 9122f31e on the Frontier install (build matched): the landing-pad
shared-pair rule, merged with its stencil guard and a pad window of 400.
Sean: the landing pad was still not taken into the layer, and the sun glare
was no longer pinned -- it moved with the head again in vivid and in
realistic.

- The rule's first pad and radar draws were classed right: `shared pair:`
  pad 2908 and radar 6658 per 30 s, world 1188-4784, stencil refusals 0;
  its first pad draw had a console draw 140 back at stencil 4, its first
  radar draw a radar family 2 back.
- The glare train was not (found on review the same day). The glare fix
  claimed exactly the WORLD class: "sun glare steady" 0.23, 0.61 and 1.94 a
  frame in the windows where world read 1188, 1610 and 4784, and nothing
  from 16:30:50, when world read 0 and radar 4574-6658 per 30 s. Sean's
  glare test fell there (vivid -> stock -> vivid at 16:32:01-05). With the
  radar up the glare train was classed RADAR and went unclaimed; the
  stencil guard could not catch it (stencil 4 is state inherited from the
  HUD's draws). Why the layer did not take the pad was not found.
- Reverted in bad1da80 (Sean: "it's up so rarely") and installed on
  Frontier. Flight 164011 on bad1da80 (build matched; no `shared pair:`
  lines, glare vivid, world variant created): the glare pinned again (Sean:
  "That fixed the sun glare").
- ruled out: the shared-pair rule as wired in 9122f31e, because flight
  162819 unpinned the sun glare in vivid and realistic (its RADAR class
  took the glare train whenever the radar was up) and did not take the pad.
- For any later attempt: a census with the sun in view and the radar up,
  and a rig that drives the real draw path (the glare verdict and the
  layer's decision over a recorded frame), not a scan of the wiring.
