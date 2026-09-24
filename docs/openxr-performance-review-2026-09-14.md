# Native OpenXR performance review — September 14, 2026

## Status

*Added 2026-09-24. Restates the journal below; update it whenever this doc
changes.*

- **State:** built on main 2026-09-24, NOT FLOWN (entry "2026-09-24: issue
  #38 and the five gaps"): a per-cycle long-frame breakdown, p99/max, late
  frames and producer-copy GPU timing; the second Submit no longer holds
  Elite through compose and xrEndFrame (`frame_end_overlap`); the XR owner
  and pacer threads run at THREAD_PRIORITY_HIGHEST; performance settings and
  Meta metrics are requested when a runtime offers them.
- **Open:** sections 5 and 6 below (the private copy, the producer copy)
  wait on `native_producer_gpu`: build either only if a copy costs more than
  about 0.1 ms per eye. The depth layer is set aside (Sean, 2026-09-24). No
  controlled comparison with the old OpenVR path exists; one now needs a
  v0.16.2 build.
- **Ruled out:** at the end of the 2026-09-24 entry.
- **Next flight:** Frontier install, one session: 5+ minutes steady in a
  ship, then a station. Pimax through SteamVR's OpenXR runtime if possible
  (issue #38's configuration). Read the lines under "What the flight reads".
- **Environment:** the numbers in the entry are Pimax Crystal Super, 90 Hz,
  separate device: Pimax OpenXR at 2600x2514, SteamVR OpenXR (`aapvr`) at
  4100x4050 and 2665x2087.

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
