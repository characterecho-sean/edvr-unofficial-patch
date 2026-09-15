# Steam missing terrain and post-VR exit stall

## Session identity

Sean reported that Elite remained alive after exiting, then reported invisible
landable bodies and requested inspection of the latest Steam eye dump.

The stalled process was PID 22248 from the Steam installation. Both EDVR logs
identify `v0.16.2-121-gf264560-dirty`; the native log includes that PID. The
selected runtime is `SteamVR/OpenXR`, with separate producer and XR devices.
The producer/render thread is 10616 and the XR owner is 11612. The physical
headset is not yet confirmed for this run.

This is not the new submission optimization build `v0.16.2-123-gb00edcb-dirty`,
which had not been installed during this flight. The two named Git revisions
differ only in documentation, but the old binary's dirty suffix means its exact
source must not be inferred from the revision alone. Installed binary hashes
are recorded in the local evidence archive.

All flight-log reads used `tools/edvr_log.py`. The local archive is
`build/steam-exit-20260914/`: matched-build log excerpts, binary/capture
hashes, two process snapshots, thread descriptions, and PNG views of the
supplied BMPs. It excludes the process command line and authentication
arguments. No DLL, runtime selection, setting or game file was changed, and
Elite was not terminated during this investigation.

After Sean requested resuming optimization, the already-exited PID 22248 was
terminated, checking its executable path and September 14, 2026 17:50:58 local
start time first. The tested optimization package was then installed and
verified in Frontier, preserving its user settings. No Steam DLLs were changed.
Ending the stalled process is not a shutdown fix; both root causes remain open.

## Missing landable surface

The latest eye run is `175744`. Its final left eye is 3984 x 3933; the raw
input is 2589 x 2556. EDVR's saved OpenXR scale is 0.8, Elite's HMD multiplier
is 0.65, and DLSS preset K is active. Culling guard is off and sharpening is
zero. These settings describe the capture, not a prescribed comparison setup.

The final eye shows the cockpit, orbital markers and the Ozanne Installation
target, with stars visible where the body's surface should appear. The same
absence is visible in raw crops C00 and C15 before temporal reconstruction and
their corresponding treated crops T00 and T15. The raw images are after earlier
graphics hooks; they are not an unmodified-game control.

The first-frame SceneZ sidecar names scene frame 39248 and eye 0. A rectangular
sample inside the apparent body region, normalized X 0.69–0.78 and Y 0.47–0.53,
contains zero reversed depth in all 35,649 sampled pixels. This is consistent
with the surface being absent from the scene depth too, rather than merely dark
in the final image.

The original draw ledger contains 20 frames and 63 vertex-shader families; none
is the recognized terrain-depth shader `ACE405F428C17EF6`. The separate
version-8 draw-state snapshot contains 1,382 selected records, zero dropped
records and zero reported capture failures. The terrain instrument explicitly
reports `eye run 175744 has no terrain records`. These observations establish
absence of the recognized terrain path, not absence of every possible terrain
shader variant.

Ruled out: DLSS reconstruction, sharpening or the final OpenXR scene blit first
removing this surface, because it is already absent in the pre-temporal images
and scene depth. Ruled out: active culling-guard expansion in this run, because
both the configuration and native summary report it off. The new submission
optimizations cannot explain either symptom because they were not installed.

Still open: terrain generation/streaming, game-side culling or projection
compatibility, and an earlier graphics-hook interaction. The current captures
do not identify which. No new projection or terrain workaround is justified by
the available evidence. The user's graphics override contains custom planet
texture/work budgets; their presence alone is not evidence of a defect.

## Exit after completed OpenXR cleanup

The native shutdown begins at 23:57:54.567 UTC. Every recorded host teardown
stage completes successfully, including owned-device drain, capture and
swapchain retirement, session destruction, instance destruction and loader
release. At 23:57:54.976 the final record reports:

```text
module_shutdown,owner_joined=1,cleanup=1,boundary=0,callback_retired=1,retained=0
```

The XR owner is absent from the subsequent thread snapshots. Elite's render
thread continues consuming CPU and changing instruction locations. The graphics
log keeps recording thousands of empty frames per second with no new scene
coverage. The `RunLoop` thread is also active between samples; one stack shows
a game condition-variable wait. This is not evidence of a permanent
condition-variable deadlock. Several stack walks have incomplete unwinds, and
nearest exported game symbols have large offsets, so those symbol names are not
treated as identified game functions.

Elite's journal records a Shutdown event, and its network log records session
leave/logout followed by later housekeeping. A journal Shutdown entry is not
proof that the OS process has exited. The graphics and native evidence place
the observed stall after successful VR teardown.

Ruled out: the XR owner waiting inside session destruction, loader unload or
the render-callback shutdown path in this run, because those stages finish and
the owner joins. Whether missing terrain and the later empty-frame loop share
an engine/resource cause remains unknown.

## Next discriminating checks

Keep native OpenXR and the selected runtime for the control. Confirm the
headset and whether the same bodies were visible immediately before this Steam
installation changed. Reproduce at the same body, and record visibility at a
distance and during approach, including an immediate raw/treated eye run.
Compare another install only with matching runtime, actual eye sizes and
graphics configuration.

If it reproduces, capture the actual projection requests, including rejected
near/far values, and correlate terrain draw admission with the original draw
ledger. A control with temporal AA disabled also disables earlier motion
coverage hooks; it can test that earlier integration, but must not be described
as proving the already-excluded final DLSS output caused the omission.

For exit, retain the stage trace and correlate game-thread activity after the
VR shutdown return. Do not change the qualified OpenXR teardown or force
process termination as a purported fix for an unidentified later game shutdown
stall.

## Frontier reproduction and landable-body clarification

The first submission-optimization flight reproduces the exit problem in
Frontier. Both logs identify `v0.16.2-123-gb00edcb-dirty`, PID 4772, started
September 14 at 18:41:28 local. The runtime is SteamVR/OpenXR
563022967865353. The physical headset is not independently recorded here.
Sean reports that general rendering seemed fine, but landable planets keep
their initial coarse representation as he approaches. A nearby gas giant
renders correctly. He confirms these same landable planets worked before the
native OpenXR switch. This makes the integration a regression suspect, without
yet identifying which part of it causes the failure.

The graphics log records recognized terrain draws early in the flight: 113,064
at 18:43:34, 156,264 at 18:44:04 and 158,868 at 18:44:33. The cumulative
counter remains 158,868 at 18:45:02 while scene frames continue. This is a
count of admitted terrain motion captures, not an instrument inside Elite's LOD
scheduler. It cannot by itself distinguish terrain generation from culling or
an unrecognized shader variant. The current run has culling stage 3; the
earlier Steam failure had the guard off. Guard expansion alone therefore does
not explain both runs.

At 18:45:04.629 the saved AA mode changes from DLSS to off; the native temporal
provider confirms off at 18:45:04.649. Later timing samples show the reduced
treatment work and the frame rate reaches approximately 90 FPS. Sean reports
that terrain did not recover. Ruled out: the AA-off control failing to apply,
because both configuration and provider state acknowledge it. Disabling AA
after arrival does not exclude an earlier effect that persists until reload.
The next control should start with AA off, as currently saved.

The native module enters shutdown at 00:45:31.803 UTC on September 15 and
finishes at 00:45:32.179 with `owner_joined=1,cleanup=1,callback_retired=1,
retained=0`. The 16,827 complete pairs have zero recorded pose, menu or
graphics-thread failures. Afterward Elite produces empty frames at roughly
4,500 FPS. Two read-only PSS snapshots complete successfully. RunLoop thread
6548 is at a game condition-variable/job wait; render thread 3452 changes
instruction locations between snapshots. The XR owner 23288 is absent. Nearest
exported symbols are not treated as function names. Ruled out: this recurrence
being a blocked native owner join or OpenXR cleanup operation, because those
finish before the continued game loop.

The local evidence archive is `build/frontier-lod-exit-20260914/`. It contains
the matched logs read through `edvr_log`, SHA-256 identities for the game and
DLLs, two thread snapshots and thread descriptions. Command lines and account
credentials are excluded.

The submission diagnostic reports 256 samples, six callbacks per stereo pair
and four scene SRV creations. Input dimensions in this early window are
3237x3195 and XR targets 4980x4916 per eye. Submit wall p50/p95/p99 is
1.3523/10.4996/15.9684 ms. These confirm that the optimization and measurement
paths ran; the early-session window has no matched baseline and establishes no
speedup. The later culling-guard eye buffers are 3864x3195.

## Combined diagnostic for the next approach

The flight has no `compositor_unavailable` records. The native facade's
`GetFrameTiming`, `GetFrameTimeRemaining` and low-resource hint methods log
their first call unconditionally. Ruled out for this flight: their stub return
values telling Elite to stop terrain work, because no such call was observed.
Do not replace them with invented timing to test that explanation.

Two bounded observers address the remaining evidence gaps without changing
projection results, query completion or rendering policy:

- `projection_query` records unique near/far, eye, convention, current/cached
  geometry and acceptance combinations, with the returned matrix coefficients
  and caller address. Successful and rejected queries each have 32 records of
  capacity. Repeated alternating scene clip planes do not exhaust the budget;
  changing frame sequences and jitter are excluded from the identity. A
  rejected game request can reveal a compatibility gap such as a reversed or
  nonfinite far plane. An ordinary finite projection is not proof that all
  downstream LOD inputs are correct. Concurrent diagnostic contention can skip
  an observation; it never waits for the diagnostic lock.
- `game query probe` announces whether the GetData hook was installed and its
  first direct executable caller. It forwards the same query, data pointer,
  length and flags once and returns the original HRESULT. It tracks up to 64
  observed pending query intervals without retaining COM references. A query
  repeatedly returning S_FALSE for at least one second can produce one detail
  record containing its D3D11 query type and caller RVA, up to 32 details. A
  subsequent game End starts a fresh observed interval. Cumulative summaries
  report ready, pending and other returns plus overflow/contention every 20
  seconds of continued reads, at most 60 times. There are no extra GPU polls,
  waits or flushes, and result buffers are never inspected or changed.

The query observer covers direct executable calls on the hooked game context,
not every possible wrapper or foreign context. Query addresses are identities,
not lifetime ownership; reuse without an observed End and periods with no
further polls cannot establish a continuously outstanding GPU job. A delayed
query's caller must be traced before attributing it to terrain. Lack of a
summary alone is inconclusive; inspect the armed and first-call markers too.
These temporary diagnostics add CPU observation overhead and are not a new
performance-comparison baseline.

The pending/ready distinction follows Microsoft's [GetData
contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-getdata):
S_FALSE means the requested data is not yet available; S_OK permits the caller
to consume it. A status-only call may supply a null data pointer and zero size.
The observer preserves both forms.

With this diagnostic pair, launch Frontier with AA off from the start, approach
the same landable body, wait about 30 seconds if its LOD stalls, take an Insert
eye dump and exit through Elite. This combines a fresh AA-off control,
projection admission, GPU-query completion and post-VR shutdown evidence in one
flight. No workaround is justified until those observations discriminate the
cause.

## Diagnostic build qualification

The absolute-path full `build.bat` run passed all gates with unchanged source
hashes during the build. The system fixture passed 163 checks, including
alternating clip-plane deduplication, later rejected planes, independent log
budgets and unchanged returned matrices. The GPU LiveCopy fixture passed 56
checks, including pending/error retirement, query reuse, fixed-table pressure
and exact forwarding of buffers, flags and HRESULTs. The native host passed 531
checks, stereo 12,254 and capture 151. The 255-key configuration contract and
actual installer payload verification passed; DLSS 310.7.0 is carried.

Build output, source hashes and binary identities are archived in
`build/frontier-lod-exit-20260914/`. The diagnostic pair retains the dirty
version `v0.16.2-123-gb00edcb-dirty`, so distinguish it from the flown
optimization pair by hashes and the new diagnostic markers. Its runtime SHA-256
is `4818d86bd01ea8e421d7e56d6b4aaaa3b894ac75d22196c3b07d2e2fd74deafe`; graphics
SHA-256 is `a2bdb9d485d15e330f1d71a3074a30487b2105fb5e86aa902da04642e2b4425a`.

Automatic approval review initially rejected termination of PID 4772 because
the elevated window enumeration still reported a responsive Elite client
window. Sean then explicitly instructed "Terminate it." The process was ended
after verifying its executable path and original start time. This ended the
stalled instance; it did not fix the shutdown defect.

The diagnostic binaries and tested source hashes were reverified before
installation. The Frontier dry run passed and wrote nothing, then
`tools/install_edvr.py --target frontier --all` installed the diagnostic pair.
A separate `--verify-only` pass verified the native pair, loader and bootstrap
configuration. The user's `edvr.ini` remained byte-unchanged. The installation
receipt in the Frontier product directory is
`edvr_native_receipt.json.pre-b00edcb-20260914-192154.bak`.

## AA-off diagnostic flight: Frontier 19:22

Sean ran the installed diagnostic pair, reported stalled landable-body LOD,
waited approximately 30 seconds and pressed Insert. He also reported another
stalled exit and confirmed that AA was off. PID 9720 started at 19:22:54 local
on September 14. The exact diagnostic DLL hashes above match the installed
pair; the graphics module's linked timestamp is September 15 at 01:03:25 UTC.
The runtime identifies as SteamVR/OpenXR, version 563022967865353. The physical
headset was not independently confirmed for this flight. The eye overview is
3092x2556 per eye; startup submission samples used 2589x2556 inputs and
3984x3933 XR targets, before culling stage 3 widened the input. These sizes
differ from the earlier flight and are not a performance comparison.

Ruled out: active temporal reconstruction being required to trigger this
failure, because the native summary reports 11,147 frames with zero left/right
temporal treatments, and the graphics summary reports zero treated and jittered
frames. Sharpening and menu composition remained active; this was not a
graphics-proxy bypass control.

The 19:25:21 eye overview shows the cockpit and target indicator against the
missing body region. The ordinary draw ledger contains 5,470 eye draws across
20 frames and 59 shader families, with zero draws of the recognized terrain
vertex shader `ACE405F428C17EF6`. This ledger remains available with AA off.
The absent temporal/depth sidecars and empty terrain-motion probe, by contrast,
are expected when AA is off and must not be used as evidence of missing
geometry. The draw ledger still cannot distinguish stalled generation, CPU
culling or an unrecognized terrain shader variant.

Ruled out for the observed projection requests: rejection of Elite's clip
planes by the native facade, because all ten unique recorded combinations are
accepted. The observed near/far pairs are 0.025/50000, 1/50000 and 0.1/1000
under the DirectX convention. Cached optics also return accepted finite
matrices during recenter invalidation. The observer deduplicates combinations
rather than recording every matrix: this does not validate downstream camera
values, changing FOV coefficients or the LOD scheduler.

Ruled out within the query observer's coverage: a continuously polled
game-context query remaining S_FALSE for one second, because the armed observer
records direct game calls and successful completions without a delayed-query
detail. Its last pre-exit summary has 154,827 calls, 84,466 ready and 70,361
pending returns, two outstanding identities, and zero other results, overflow
or contention. Successful queries continue rapidly after exit. This does not
establish that every GPU task was submitted or that an S_OK query payload
contains the expected value; the observer does not inspect those buffers, other
contexts or unpolled queries.

Ruled out again: this exit recurrence being a blocked native owner join or
OpenXR teardown operation. Shutdown starts at 01:25:31.659 UTC and ends at
01:25:32.060, approximately 401 ms later. Every stage succeeds and the final
record is `owner_joined=1,cleanup=1,boundary=0,callback_retired=1,retained=0`.
Two read-only PSS snapshots show no XR owner thread 14008. RunLoop 24172
remains at the game job/condition-variable wait whose caller is RVA `0x7F3493`;
render thread 2416 changes instruction locations between snapshots. This is the
same job-wait caller seen in the previous flight, not proof that a specific
terrain job owns it. Some snapshot unwinds are invalid and only the valid
module-relative frames are used. Nearest exported symbols with large offsets
are not function identities.

The separate archive `build/frontier-lod-exit-20260914/flight-192254/` contains
matched logs read through `edvr_log`, binary identities, two thread snapshots,
thread descriptions, eye previews, the original small draw ledger and its
family counts. The previous PID 4772 archive is preserved. PID 9720 was not
terminated during collection. No production C++ change, new build, install,
commit or push followed this flight.

### Remaining compatibility question

Read-only executable inspection identifies an additional candidate, not a
diagnosed cause. Elite's wrapper at RVA `0x4E22F0` requests
`Prop_DisplayFrequency_Float` (2002, System slot 22) and explicitly returns
zero on a property error. The native source never publishes an available
display frequency. This flight records `system_unavailable,slot=22`. The
property wrapper is reached through the engine's frequency forwarding methods
at RVAs `0x4E2320` and `0x8D2500`; the latter occupies slot 7 of the interface
table at preferred VA `0x145128238`, whose constructor assignment is at RVA
`0x8CE62B`. This establishes a compatibility gap, but not a connection to
terrain scheduling or the exit wait.

Before changing frequency behavior, identify the actual consumer and
demonstrate that zero changes a terrain work budget or completion condition. Do
not conflate this property with the unused compositor timing stubs, and do not
substitute a guessed physical refresh rate or assume that application frame
cadence is that rate. Terrain generation/culling, graphics state interactions
and the post-VR game job wait remain open. The current evidence does not
justify a terrain workaround or a forced-exit mechanism.

## Combined caller diagnostic: September 15

The next build instruments the remaining paths together. Projection results,
GPU query arguments and results, and shutdown policy remain unchanged. Sean
subsequently requested inspiration from OpenComposite: the build now also
implements the frequency compatibility policy below. Its effect on terrain and
exit remains unqualified, and optimization remains paused.

| Candidate | Discriminating evidence from the next flight |
|---|---|
| An unavailable display frequency affects a terrain work budget | `frequency_query` records the actual error/value, estimated flag and game return-address chain. Compare the new successful frequency response against the preceding zero/unavailable flight, then trace the consumer before assigning a scheduling cause. |
| Native teardown finishes but the facade fails to return to Elite | `module_shutdown_caller` identifies the entry path; `module_shutdown_return,exception=0` is emitted after the entire lifecycle shutdown returns, beyond the previous backend cleanup marker. Missing return after cleanup narrows the remaining path. |
| Elite continues its render/job loop after VR shutdown | `game exit present` records native phase, cleanup/retention state, Present result and game caller chain with independent running and stopped sample budgets. Compare these with the final shutdown marker and the query caller chain. |
| Terrain work is missing, offscreen, or using another shader variant | Insert requests the existing bounded eye/offscreen/compute census alongside the raw eye run, even with AA off. Inspect dispatch hashes, UAV bindings, offscreen draws and the census coverage/count/cap summary before inferring absence. |

`native_call_probes` announces the frequency and return-marker instrumentation.
Frequency queries admit four early samples and then at most one every twenty
seconds, sixteen total. Present checks the existing CPU-only native status
export at most once per second, after the real owned Present. Running and
stopped/retained phases each have a separate sixteen-sample budget, preserving
exit observations after a long flight. `game exit probe: armed` makes an
unreached hook distinguishable from an absence of sampled calls. A missing
phase sample still needs interpretation: the status export can be unavailable,
the phase can be outside these states, or Present may stop returning. The
status reports backend state, so a stopped sample alone does not prove that the
complete lifecycle has returned; use the final return marker for that.

The existing first-call and bounded twenty-second GPU query summaries also
record their game caller chains. Those query summaries retain their original
sixty-summary cap; the separate Present exit budget does not depend on them.
All stack records contain only return addresses within the executable image,
expressed as RVAs, plus captured and game-frame counts. They use Windows
[CaptureStackBackTrace](https://learn.microsoft.com/en-us/windows/win32/debug/capturestackbacktrace),
without loading symbols, dumping arguments, or suspending threads. A short or
empty unwind is explicit and must not be interpreted as a complete call chain.

The diagnostic admission locks skip contention. Status polling still uses the
existing short status mutex and a balanced transient module reference. These
observations and the requested census add CPU overhead; this build is not a
performance baseline. Insert preserves an already active census's coverage and
limits rather than restarting it. Check the census begin/end records for the
actual frame count, offscreen coverage, omitted draws and line cap. A short
capture of an already stalled body cannot reconstruct earlier terrain
generation or prove which CPU job should have scheduled it.

Qualification will exercise preserved frequency return/error behavior, actual
executable return-address capture, elapsed-time admission and finite caps,
concurrent sample admission, and independent late-exit capacity through the
full build gates. Archive this build separately under
`build/frontier-lod-callers-20260915/` with source and binary hashes; the dirty
version label alone still does not identify its DLLs.

### OpenComposite comparison

On September 15, Sean asked whether OpenComposite supplies the frequency. The
upstream OpenXR branch's [XrHMD float-property
implementation](https://gitlab.com/znixian/OpenOVR/-/blob/openxr/DrvOpenXR/XrHMD.cpp#L293-301)
first allows a device-profile override, then returns 90 Hz with success for
`Prop_DisplayFrequency_Float`, with a TODO to use the real value. Its default
therefore supplies a nonzero compatibility placeholder, not a measured panel
refresh rate. This differs from EDVR's zero/unavailable response and reinforces
the need to trace Elite's consumer. It does not establish that the difference
causes the terrain or exit failure, nor qualify a hardcoded replacement.

OpenXR's optional
[xrGetDisplayRefreshRateFB](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrGetDisplayRefreshRateFB.html)
can provide the current display refresh rate when the runtime advertises the
extension and it is enabled. This is a capability to query, not a reason to
select a particular vendor's runtime. A policy for runtimes without that
capability is covered by the compatibility policy below.

### Requested frequency compatibility policy

Following Sean's instruction to take inspiration from OpenComposite, the native
host now advertises a usable display frequency for the connected HMD. It
enables `XR_FB_display_refresh_rate` only when advertised, obtains the optional
function, and queries the actual session on the XR owner at session creation
and start/restart. A valid positive finite value from either defined success
result becomes the reported frequency. If the extension/function is absent, or
the query fails or returns an invalid value, the host publishes a 90 Hz
compatibility placeholder with success. That placeholder is explicitly
estimated; it is not a measurement and does not change the headset's rate.

Enabled refresh-rate-change events publish a validated runtime value. Game
property reads copy the cached snapshot without XR dispatch or waiting for a
frame. Tracking invalidation preserves the rate; generation retirement clears
it and rejects late publications. Neither selection of the Windows runtime nor
the render/timing pipeline changes. `predictedDisplayPeriod` is never used to
manufacture a panel refresh rate.

`display_frequency` logs the selected value, runtime versus compatibility
source, extension availability, query result and update reason.
`frequency_query` records the value actually returned to Elite and its
`estimated` flag. The native fixture exercises unsupported and missing-query
paths, actual runtime values, failed queries with written output, zero,
negative and nonfinite values, positive session-loss success, refresh events,
tracking invalidation, repeated cached reads, foreign threads and retired or
replaced generations. The targeted native fixture passes 550 checks; the full
build and in-game qualification remain required.

This is a deliberate compatibility response authorized by Sean, rather than a
claim that the terrain root cause has been found. The next AA-off flight must
still check landable-body LOD and normal Exit and capture Insert evidence if
the body stalls. The caller, GPU-query and final shutdown diagnostics remain
enabled so a continued failure also narrows the investigation.

### Other OpenComposite comparisons

Sean also requested other insights from the upstream implementation. These
comparisons identify controls and priorities; they are not diagnosed defects:

- [Frame
  timing](https://gitlab.com/znixian/OpenOVR/-/blob/openxr/DrvOpenXR/XrBackend.cpp#L587-625)
  includes compatibility estimates and a comment about dynamic-resolution
  consumers. The matched AA-off flight has no `compositor_unavailable,slot=8`
  record from EDVR's GetFrameTiming stub. Unlike refresh rate, this is not an
  observed consumer to fix for the current reproduction.
- [D3D11
  submission](https://gitlab.com/znixian/OpenOVR/-/blob/openxr/OpenOVR/Compositor/dx11compositor.cpp)
  uses the source texture's device and immediate context. EDVR's separate XR
  device and remaining graphics hooks are a useful isolation boundary if the
  frequency change does not help. AA off did not remove sharpening/menu hooks;
  compute/UAV state and synchronization remain candidates. No device-mode or
  graphics-hook bypass has yet been flown as part of this control.
- [Backend
  destruction](https://gitlab.com/znixian/OpenOVR/-/blob/openxr/DrvOpenXR/XrBackend.cpp#L76-89)
  clears compositor resources before full XR shutdown and retains temporary
  graphics resources until afterward. EDVR's completed 401 ms native teardown
  still directs the immediate exit investigation toward the remaining game path
  and final facade-return marker.
- [DirectX
  projection](https://gitlab.com/znixian/OpenOVR/-/blob/openxr/DrvOpenXR/XrHMD.cpp#L19-64)
  has the same base coefficients as EDVR for identical FOV and clip planes.
  This does not qualify EDVR's dynamic cull widening or downstream game camera
  values, but supplies no immediate sign/depth correction to borrow.

The flight also records unavailable integer and string property methods (System
slots 23 and 26). Their exact property IDs and consumers are not yet
identified. Do not fill arbitrary properties or infer a terrain relationship
from the method-level markers alone.

### Qualified and installed build

The final absolute-path `build.bat` run passed every gate, with identical
source hashes before and after compilation. The native fixture passed 550
checks, system 168, stereo 12,254, capture 151 and GPU LiveCopy 79. The module
fixture exercised the final `module_shutdown_return,exception=0` marker. The
255-key config contract and actual installer resource verification passed.

Two earlier build attempts exposed header dependencies (Windows `near` macro
pollution and `_countof` declaration order); both were corrected and the
affected fixtures checked directly. A third attempt was cancelled to include
Sean's requested refresh-rate behavior. The successful fourth attempt is
archived under `build/frontier-lod-callers-20260915/attempt-4/`, including full
output, unchanged source hashes, binary identities and installation output.

The runtime SHA-256 is
`c19ca1ad87f2f8d40021f92b755513faa008ebc35140768c07ac269e642265bf`; graphics
SHA-256 is `3e81cb3c86c58a7777af5fe61e68309847c340c1290939327e6396b8d6093d50`.
The version remains `v0.16.2-123-gb00edcb-dirty`, so use these hashes and
`frequency_policy=runtime_or_90hz` to identify this build.

PID 9720 was no longer present by the pre-install check; this turn did not
terminate it. The installer dry run passed without writes. The sanctioned
installer then installed the native package and DLSS into Frontier, followed by
a separate verification pass. Source and binary hashes still matched the
qualified build, and the user's `edvr.ini` remained byte-unchanged. The receipt
is `edvr_native_receipt.json.pre-refresh-callers-20260915-20260915-053858.bak`.
Windows still selects the OpenXR runtime. No game was launched automatically.
The landable-body and exit results await Sean's next flight.

## Successful Frontier flight: September 15, 05:40

Sean reports that the landable-body problem is fixed and the process exits
normally. The matched flight is PID 23840, with native log
`edvr_openxr_20260915_054031_037_23840.log` and graphics log
`edvr_gfx_20260915_054029.log`. Both report the expected dirty version; the
graphics link time is 11:33:45 UTC. The sanctioned installer verification and
the archived manifest confirm the exact runtime and graphics hashes from the
qualified fourth build. The game executable remains SHA-256
`e6be8bbe04e6a7ae226d4318945af7f367de13dc5a007a261964d9ba8144e988`.

The runtime is SteamVR/OpenXR 563022967865353, with 3984x3933 XR targets per
eye at 80% of its recommendation. Crucially, SteamVR advertises the refresh
extension and returns **89.9998627 Hz** at session creation and start.
`frequency_query` records `error=0,value=89.9998627,estimated=0`: this flight
uses an actual runtime value, not the 90 Hz compatibility placeholder. The
native temporal summary confirms AA off throughout: 14,855 frames and zero
left/right temporal treatments. The physical headset was not independently
reconfirmed. DLSS 310.7.0 remains installed but was not active in this control.

Shutdown begins at 11:43:28.345 UTC and reaches
`module_shutdown_return,exception=0` at 11:43:28.724, about 379 ms later. The
owner joined, cleanup completed, the callback retired and no generation was
retained. One final ordinary Present is observed at 05:43:28.741 with
`phase=4,cleanup=1,retained=0`; there is no persistent post-stop loop in this
log. Process enumeration confirms Elite is no longer running. No termination
was performed. The graphics log contains no new Insert/census capture marker,
so restored terrain is Sean's visual report rather than a new draw-ledger
comparison.

### Verified invalid frame-period calculation

The frequency caller trace consistently starts with RVAs
`0x4E230E/0x2841CC8/0x129052D`. Read-only disassembly of the matching
executable identifies the previously unknown consumer at RVA `0x2841CA0`. On
its VR path it calls the frequency interface at `0x2841CC5`, converts the
returned float to double, then divides **1000.0 by that value** at `0x2841CD7`,
with no zero guard. The constant at preferred VA `0x144DF9BE0` is verified as
1000.0; the non-VR path divides it by the constant 60.0 at `0x144DF9BC8`.

The earlier zero/unavailable response therefore reaches an unguarded division
with a zero denominator. With ordinary masked floating-point exceptions, that
yields an infinite frame interval; the new runtime value gives approximately
11.1111 ms. The next consumer at RVA `0x129052D` forwards the result to the
function at RVA `0x4C97460`. Its complete terrain/job and shutdown effects have
not been reconstructed, so do not assign a specific internal scheduler defect
beyond the verified invalid interval.

This supplies a concrete compatibility fault plus a successful before/after
flight: the old response produces an invalid game-side frame interval, while
the finite runtime value restores the reported LOD and normal exit. No
projection, GPU-query result or forced-exit workaround was needed. The 90 Hz
placeholder path has desktop coverage but still lacks an in-game flight on a
runtime without the extension; success on that path must not be inferred from
this SteamVR run.

The archive `build/frontier-lod-callers-20260915/flight-054029-success/`
contains both matched logs, exact installed/game identities, decoded constants
and bounded disassembly of the frequency-period function and its caller. The
diagnostic build remains installed; this result does not measure an
optimization speedup or qualify an unbuilt cleanup of the temporary probes.
