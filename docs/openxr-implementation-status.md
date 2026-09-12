# OpenXR implementation status

The approved design in [openxr-port.md](openxr-port.md) was pushed to main as
`8c617dc` before implementation began. Work remains in Phase 0; it is not an
OpenXR backend or a completed Phase 0 qualification. The latest checkpoint is
the [render-to-submit measurement](render-to-submit-gpu-2026-09-12.md),
following the successful Frontier timer-migration check. Earlier review
decisions below record the state at those checkpoints.

## Implemented evidence tools

- The original shipping proxy remains the default. The new startup-only
  `advanced.openvr_census = on` setting enables typed forwarding for the exact
  four historical interfaces. The 84 methods use Valve v0.9.20 declarations;
  aggregate returns are ordinary C++ member calls. The bounded cache preserves
  repeated-getter wrapper identity and passes through when exhausted. Runtime
  targets remain owned by the runtime.
- The ABI log records the first four calls per method and caller category, with
  QPC and thread ID. The call site is classified by its containing module: game
  executable, EDVR, another module, or unknown. Calls EDVR makes directly to
  saved runtime pointers bypass these wrappers. Other interface versions retain
  proxy behaviour. This instrumentation does not define the future owned
  backend's supported-interface policy.
- CPU order logs record at most 64 observations per wait/submit/Present event
  kind and 16 per intercepted GPU-command kind, independently for startup and
  the initial VR capture. The owned compositor's first pose wait selects the VR
  bank locally and requests the same switch from the paired graphics DLL. The
  receiver acknowledges only when its census is enabled; missing, older or
  uninitialized receivers are retried on at most 64 pose waits, with explicit
  pending/exhausted diagnostics. Repeated requests cannot refill the budgets.
  The bridge does not initialize graphics, load a DLL or issue GPU work. The
  records include phase, actual context pointers, immediate/deferred type,
  thread IDs and QPC. The two DLLs have independent ordinals; use QPC to
  compare them. A command entry is a CPU observation, not evidence of GPU
  completion. Existing unhooked commands are outside this capture.
- Device creation logs include adapter LUID, feature level and the published
  first-device identity. Validated eye submissions log texture/device identity,
  full descriptors, colour space and submit flags before EDVR substitutes a
  texture. Changes are bounded to 16 lines; unchanged handles are resampled no
  more often than every six seconds. The initial unvalidated submissions and
  skybox resources are not covered by this descriptor probe.
- `build\openxr_probe.exe --loader C:\absolute\openxr_loader.dll` loads a
  trusted, explicitly selected loader with restricted dependency search. It
  reports extensions, runtime identity, HMD system limits, stereo view sizes,
  blend modes and D3D11 adapter/feature-level requirements. It requests OpenXR
  1.0, creates an instance, and destroys it on exit. It creates no session,
  device or swapchain and does not change runtime selection. The declarations
  are pinned to Khronos SDK 1.1.46 with the upstream license retained.

Set the census key in each proxy's applicable INI before launch. Use
`tools/install_edvr.py` for installation and `tools/edvr_log.py` to retrieve
logs and verify build identity. The selected test installation is Frontier.
Preserve its tuned INI and runtime choice when enabling the census; use a
backup and review the single-key change before installing it. Steam is not the
test target.

## Review decisions and remaining gates

The GPU timing draft failed review: it permitted overlapping outer scopes,
accepted deferred contexts, recreated queries instead of reusing them, aged
samples by API-call count and mishandled zero-frequency results. It and its
insufficient tests were excluded. No new GPU queries or Monitor values were
enabled at that initial checkpoint. Rebuild the bracket after an
ownership/order capture, with the single outer scope and failure tests
specified in the approved plan.

The [first Frontier census flight](openxr-flight-2026-09-11.md), using build
`070e49d`, observed 19 methods, consistent device identity for all 16 sampled
eye textures, and multi-threaded System calls before compositor startup. The
startup Present budget ended before the first pose wait, so the initial VR
capture bank was added. This flight supports the published-device candidate for
that run, but does not qualify an overlapping graphics/VR frame bracket.

The repeat Frontier flight on `2a56da3` passed the overlap check: both DLLs
selected their VR banks on the first pose wait, and all 32 captured stereo
pairs followed wait, left submit, right submit, Present on one thread. All
sampled commands used one immediate context, and all 16 sampled eye textures
again matched the published device. There is measured work after Submit and
even after Present; the approved render-to-submit span must exclude and name
that later work. This supports a guarded single-context prototype for the
observed configuration. It does not establish complete command coverage,
whole-session ownership, telemetry overhead or compatibility on other runtimes.
Sean reported normal visuals and tracking on the repeat flight.

The desk prototype in `src/common/gpu_span_state.h` separates the query-state
policy from the D3D11 adapter. Its contract is one outer scope, six timestamp
markers (outer start/end and two eye intervals), eight reusable slots,
immutable device/immediate-context/thread ownership, and explicit
frame-associated valid/invalid outcomes. Pending results retain their resources
until ready, failed or expired by elapsed time. Tests must derive results from
markers actually issued, rather than returning a canned valid sample when a
marker is missing. The D3D11 adapter must preserve `S_FALSE` as pending and use
`D3D11_ASYNC_GETDATA_DONOTFLUSH`; timestamp ticks are integers, and a disjoint
counter cannot produce a valid duration. See Microsoft's [GetData
contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-getdata)
and [query
definitions](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query).
This state prototype alone is not an implemented or qualified GPU measurement;
real D3D11 workloads, shared door-query ownership, game-path placement and
Monitor integration remain separate gates.

The first state-policy draft failed review because it omitted the outer end
timestamp, could close an unfinished eye pair, lost resource ownership on some
retirement paths and checked sequence freshness only against undrained slots.
Its fake driver supplied valid-looking timestamps independently of issued
commands. The retained version uses an explicit active eye, a persistent
sequence watermark, integer timestamp differences, bounded per-frame result
records, and terminal shutdown after uncertain query closure. Its fake records
each marker and checks live scopes and resource ownership. Desk tests cover
both eye orders, malformed and incomplete pairs, every timestamp failure,
creation/begin/end/poll failures, partial readiness, disjoint/invalid values,
resource reuse, ring pressure, elapsed-time expiry and owner rejection. No
shipping proxy called this prototype at that checkpoint. Native ownership,
shared-clock migration and integration were kept as subsequent gates.

The first ABI test draft used a hand-built raw vtable and crashed on a matrix
return, causing a Windows error dialog. That fixture was removed. The retained
tests use concrete C++ implementations and set Windows error mode to suppress
interactive crash dialogs.

Before treating Phase 0 as complete, still collect and review:

1. Exact init/shutdown/re-init and interface-validity export traffic,
   meaningful property/controller/event arguments and returned events, first
   geometry results, skybox descriptors and complete lifecycle behaviour. The
   current bounded method census establishes calls, not this full semantic
   inventory.
2. Real startup and flight order/context/device evidence, including texture
   reuse, both eyes, mirror work and deferred command-list execution. A missing
   line is not proof of absence. The first published device matched the sampled
   textures in both Frontier flights; other configurations and complete
   lifecycle ownership remain unverified.
3. Installed SteamVR, VDXR and Pimax runtime probe reports, then a separate
   session harness for actual formats, startup geometry, refresh rate, gaze,
   tracking and loss/focus behaviour. Extension advertisement alone is not
   functional support.
4. A corrected GPU bracket, matched-frame SteamVR correlation and the Monitor
   source/validity changes. The OpenXR backend, transport parity, field
   qualification and retirement proposal follow those gates.

The CPU policy now separates `endEye` from `finishFrame`: both EDVR eye
intervals can end before the outer marker is placed after final submit work.
Explicit rejection or an early final boundary produces an incomplete sample.
The updated command-derived tests also prove that extra work after the eye
intervals extends only the outer duration. The full build passed with 2,465 CPU
policy assertions; that count includes repeated driver/resource checks, not
2,465 independent scenarios.

The first real D3D11 adapter and shared-clock drafts failed review and runtime
tests. They were moved out of the source tree to an ignored local draft
directory and are not linked into either DLL. They require a fresh reviewed
implementation: partial-issued timestamp handling, actual OS-thread ownership,
COM/module lifetime and meaningful controlled workloads remain mandatory. The
integration audit also found disjoint clocks in the temporal, sharpening,
supersample, DLAA, menu and sampled-draw instruments; sharing only the Monitor
door clock would not resolve the overlap risk.

The game-exit interruption is resolved: the menu-worker lifetime fix in
`e9802b7` passed its Frontier exit check, and `58d1566` passed a second exit
check with no matching Windows crash record. The later findings in
`openxr-flight-2026-09-11.md` still qualify the earlier `2a56da3` run: its
capture passed the bounded ordering gate, but its process aborted in the
menu-worker destructor.

The separately reported startup black screen is also resolved on Frontier.
Build `58d1566` embeds the unchanged temporal shader bytecode compiled during
the build. Its verified first Present hook took approximately 23 ms instead of
18.365 seconds, and Sean confirmed the initial black screen is gone. See [the
startup investigation](startup-delay-2026-09-12.md). These results clear the
game startup/exit interruption; they do not qualify the rejected GPU harnesses.
The replacement adapter and shared-clock desk results are recorded in the query
foundation follow-up below. Game integration remains a separate gate.

No configuration key or existing feature has been retired.

## Query foundation follow-up

The reviewed native adapter and shared-disjoint lease policy now have required
desk-test gates. See [GPU query foundation](gpu-query-foundation-2026-09-12.md)
for ownership rules, rejected draft findings, real WARP workloads and remaining
game integration. The existing CPU frame policy still owns frame association;
these components alone do not publish a game GPU value.

The startup shader fix was separately merged and pushed to main as `ec26970`,
after a full regression build on current main. The unfinished OpenXR/GPU work
was not included in that merge.

## Main integration and next Frontier flight

Main including PR #33 is merged at `06636b2`. Both context-hook diagnostics and
the bounded VR census survived the merge, as did the startup and worker-exit
fixes. The full paired build passed. The next flight uses Pimax through SteamVR
and explicitly checks LiveCopy activation, working rendering fixes, startup,
exit and refreshed ordering evidence; see the [Frontier flight
checklist](frontier-livecopy-flight-2026-09-12.md).

The `f08098c` Frontier check passed on Pimax through SteamVR: Sean confirmed
normal on-foot/cockpit rendering, prompt intro and clean exit. Both logs match
the installed build; LiveCopy is active in both context hooks, feature counters
advance, and all 32 captured stereo pairs retain wait/both-eyes/Present order
on one immediate context and thread. The flight lasted about 3 minutes 15
seconds and included AA changes and the existing culling diagnostic, so it is
functional integration evidence rather than a controlled overhead measurement.
The shared-clock migration and later GPU accuracy/overhead gates remain open.

## Existing timer migration checkpoint

The existing production GPU timers now use the shared frequency service. See
[the migration review and regression gate](gpu-timer-migration-2026-09-12.md)
for caller coverage, inactive-producer recovery, explicit cleanup, desk results
and the next Frontier check. The render-to-submit frame instrument is still
inactive. This checkpoint must pass its Frontier regression before broader
measurement is enabled.

The `cc3d882` Frontier functional check passed: Sean reported normal behavior,
both logs match the installed checkpoint, the shared clock produced completed
and nonzero pass samples, LiveCopy remained active, and the 32 captured pairs
retained the same immediate-context owner and order. F8 was closed and
reopened, and the exited process had no matching Windows crash/hang event. The
roughly 4.5-minute run included an initial period dominated by SteamVR pose
waits, so it is not a controlled performance comparison. Full evidence and
limits are recorded in the migration document. The next implementation step is
the guarded render-to-submit span and frame-associated Monitor output; accuracy
and overhead remain separate gates.


## Render-to-submit checkpoint

The local GPU span now runs through the current paired proxies, with a
versioned CPU boundary bridge and owner-thread query operations on the shared
frequency scope. The Monitor adds a separate source/frame/age readout; existing
SteamVR readings and graphs remain intact. See the [implementation and test
record](render-to-submit-gpu-2026-09-12.md) for the exact boundary, command
coverage, review corrections, invalidation behavior and Frontier checklist. The
two inner intervals include runtime Submit and are labeled submit paths, not
EDVR-only cost. The next gate is a Frontier functional test; matched-frame
accuracy, overhead and other runtime/lifecycle checks remain open.


The clean `0ac3095` checkpoint passed the full build and production WARP smoke,
but failed the Pimax/SteamVR visual gate: Sean reported renewed shimmering
during head movement. Both flight logs and installed DLL hashes match that
checkpoint. The instrument completed samples, but the cause of the visual
regression is unresolved. The test fixtures did not exercise production
temporal rendering with the outer frame scope active.

Frontier was restored to the exact previously normal `cc3d882` DLL pair, with
both hashes verified and current INI bytes preserved. Use `--expect-build
cc3d882` for the next flight, which compares the same scene and head movement
before any speculative rendering fix. The implementation remains on this branch
and must not advance until this gate is resolved. See the [regression evidence
and rollback
record](render-to-submit-gpu-2026-09-12.md#head-movement-regression-and-rollback).
