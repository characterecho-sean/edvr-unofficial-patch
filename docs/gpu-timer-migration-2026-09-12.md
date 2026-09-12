# Existing GPU timer migration

Status: source review, full paired build and desk checks passed; Frontier
regression flight pending. This checkpoint converts existing measurements to
the shared disjoint clock. The render-to-submit frame instrument and OpenXR
transport remain inactive.

## Scope and ownership

`gpu_timing.*` supplies a reusable timestamp pair and the process's shared
frequency domain. The door, temporal, DLAA, sharpening, supersample, menu,
UI-content, stellar-coverage and celestial-motion timers use it. The
synchronous exported DLAA cost probe retains its independent desk-only clock;
the game never calls that probe. Every production disjoint allocation now goes
through the frequency-only native adapter.

The first interval owns the physical disjoint Begin/End; nested intervals
borrow its frequency and retain their own start/end markers. Closing an
unfinished parent invalidates its remaining borrowers without issuing late
timestamps. Timing pressure skips measurement while rendering continues.
Existing metric names and measured command boundaries are preserved;
shared-ring pressure can change sample availability.

The service binds the canonical immediate context, device and actual OS thread
during vScreen installation. The recorded Frontier configuration uses that
thread for creation, Present and captured submissions. A different thread or
context cannot issue timing commands. This is a guarded policy for the observed
configuration, not proof that all runtimes or device lifecycles have the same
ownership.

Timestamp polling retains `S_FALSE`, caches independently ready fields and
always uses `DONOTFLUSH`. Errors, disjoint frequency, reversed ticks and
samples older than two seconds are invalid rather than zero. A sweep on the
owner thread, at most once per 100 ms when a new sample begins, retires expired
timers even if their producer stopped running. Without that sweep, turning a
feature off with eight pending samples could permanently occupy every shared
record. The sweep releases expired leases; it does not recycle frequency
records still held by unexpired borrowers.

Temporal statistics remain separate from timestamp validity: staging readback
can complete while timing is unavailable, and a slot is reusable only after
both parts finish. An aborted pass cannot read the previous frame's staging
contents. Sampled draw timers retain their four-frame polling delay and recover
from temporary lease or record pressure.

## Lifetime and review

Timer destructors are deliberately inert. Explicit resets release timestamp
queries, and a verified owner context may cancel an unfinished interval. A
reset without that context issues no context commands; abandoning an open
sample makes measurement unavailable until explicit owner shutdown. The owner
identity remains available for statistics readback during that stopped state.
After global shutdown detaches the registry, quiescent sampler cleanup uses
`reset()` without a context.

Quiescent `FreeLibrary` cleanup detaches the domain before resetting pass
owners, with no Begin, End, GetData or Flush from teardown. Normal process
termination continues to skip this cleanup entirely, preserving the previously
verified exit fix. The registry and static timers do not perform COM cleanup
from static destruction. Explicit owner-thread shutdown remains available to
desk callers; no new loader-lock callback attempts to close GPU scopes.

Luna agents migrated the callers. Parent review corrected failed-End slots,
stale temporal metadata, permanent sampler disablement after transient
pressure, cleanup flags and the inactive-producer starvation path. The first
new test draft was rejected before execution because it closed borrowers after
the parent and expected the wrong query count. Its replacement checks issued
commands, query allocation/release balance and actual pixel output.

## Desk validation

The focused `timer-targets.log` run passed 299 shared timer lifecycle checks
and 20,938 checks in the existing production UI coverage test. Counts include
repeated assertions, not independent test scenarios. The UI test now also runs
its real sampled shader/copy work inside a shared parent interval in three
supported source formats, reads back the pixels and verifies both durations
complete. When the D3D debug layer is installed, the existing UI gate rejects
warnings/errors.

The shared timer tests cover partial allocation and null outputs, failed and
unexpected HRESULTs, partial readiness, cached frequency, reversed timestamps,
uncertain closure, record and lease pressure, disabled producers, aborted open
intervals, actual foreign OS threads, deferred contexts, distinct devices,
explicit cancellation and release-only unload. Native WARP clear/copy work uses
one disjoint query and three timestamp pairs; exact texture readback occurs
outside the measured intervals. The focused run measured 0.4426 ms outer and
0.4421 ms nested. These are software-device test results, not Elite performance
figures.

The isolated production-proxy WARP smoke passed in
`build/shared-timer-smoke.log`: view recycling/black void, supersample filters,
temporal history/cuts/eye bounds, depth readback, DLAA/DLSS fallback and
sharpening. The fixture log confirms LiveCopy in both context hooks and the
first shared-clock interval completing (4.1054 ms at frequency 10,000,000).
This source build was stamped `dd65e0f-dirty`; it is not the installed flight
checkpoint. WARP does not qualify NVIDIA NGX/DLSS or headset rendering; those
remain flight checks.

The full paired build passed in `build/shared-timer-migration-validation.log`,
including the 299 timer checks, 20,938 production UI checks, 405 terrain-motion
checks, native adapter/shared-clock/LiveCopy gates, actual paired-DLL census
bridge, OpenVR ABI tests, config contract and Python tool self-tests. The final
WARP clear/copy result was 0.4439 ms outer and 0.4434 ms nested. Test targets
that include migrated production code now link the shared helper and explicitly
clean up their timer owners before releasing WARP.

## Frontier regression gate

The clean checkpoint `cc3d882` (`v0.15.1-51-gcc3d882`) passed the full paired
build in `build/frontier-shared-timer-clean-build.log`. Its isolated production
WARP smoke also passed in `build/shared-timer-clean-smoke.log`; the versioned
fixture log confirmed a completed shared-clock interval. Both proxies were
installed and independently verified in Frontier at 07:09 local on 2026-09-12.
The subsequent documentation commit does not change the installed version.

| Payload | Linked build stamp | SHA-256 |
| --- | --- | --- |
| `d3d11.dll` | `6AA54E5C` | `746E30383B0F3EE343EF40F02A94AF7A7971673A92A52C96032FC51B77C45453` |
| `openvr_api.dll` | `6AA54E64` | `9B4CC00A98D099530998757EFB3BD89BE7958B1780F6257BAFB91DFAE7B14538` |

The INI remained byte-identical, with SHA-256
`A32D631834E5C1EEEFAA03367750A7A4211652869EB4976900DC11CF530EC1A9`. The
installer retained both previous proxies as `pre-cc3d882-20260912-070917.bak`
backups. No game was launched by this work.

This flight's expected build is `cc3d882`, even if branch HEAD has advanced:

```powershell
python tools/edvr_log.py --target frontier --tag gfx --expect-build cc3d882 --version
python tools/edvr_log.py --target frontier --tag vr --expect-build cc3d882 --version
python tools/edvr_log.py --target frontier --tag gfx --expect-build cc3d882 --grep 'GPU timing:|door GPU|GPU price|vScreen totals|unavailable|sentinel|exception'
```

Install both proxies from the clean checkpoint with `tools/install_edvr.py
--target frontier --dll --openvr`, then use `--verify-only`. Preserve the live
INI and record the installed commit and hashes. Steam is not this test's
target.

Use Pimax through SteamVR for 5-10 minutes: check prompt intro, both eyes and
tracking, cockpit and on-foot rendering, and F8 menu open/close. Leave the menu
closed for several seconds and reopen it to exercise an inactive timer
producer. Keep the existing AA settings. Exit normally and report any crash,
hang, new stutter or visual change.

After the flight, check both logs with `tools/edvr_log.py --expect-build` using
the installed commit. Require the new `GPU timing: shared disjoint clock bound`
and `GPU timing: first shared-clock interval ready` lines, compatible
owner/census identities, active LiveCopy hooks and normal existing feature
counters. Binding alone is not evidence that a sample completed. Review
existing door/pass timing summaries where those features ran and investigate
any unavailable-timing note. Missing feature output is an untested path, not a
successful measurement.

This flight is a functional regression gate for the migration. Matched-frame
SteamVR correlation, enabled/disabled overhead, frame sequence/age/validity in
Monitor, the render-to-submit boundary and subsequent OpenXR implementation
remain separate work.
