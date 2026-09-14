# Render-to-submit GPU checkpoint

This follows the successful `cc3d882` Frontier timer-migration check. It adds
the local frame measurement to the current OpenVR proxy. It does not add an
OpenXR renderer or complete Phase 0 qualification.

The `0ac3095` headset gate failed: renewed shimmering was reported during head
movement. Frontier was restored to the exact `cc3d882` DLL pair. The cause
remains unresolved; the timing implementation is not qualified for promotion.
See the regression record below.

The rollback also shimmers and blurs during head movement. Both rollback logs
and installed DLLs match `cc3d882`, and the INI still matches the earlier run.
Ruled out: the new outer frame instrument as a necessary cause, because the
symptom persists without that implementation. The report affects scene geometry
and text, and switching AA off makes the moving image clearer.

The subsequent paired captures established a separate projection mismatch under
the active raw-only culling probe. Temporal reconstruction was using the
widened raw frustum while the game rendered through its original matrix. See
the [projection diagnosis and
correction](temporal-projection-probe-2026-09-12.md).

## What the value means

The start marker precedes the first covered call on the bound immediate context
after a successful owned pose wait. The end marker follows the second distinct
accepted eye's entire submit path, including the runtime Submit call and EDVR's
post-submit shadow copy. Theater, healing and shadow-resubmit paths share the
same wrapper. Classic withholding, rejected submits, malformed pairs, missing
eyes and new boundaries cannot produce a valid completed frame.

The two inner intervals are **submit paths including runtime work**. They are
not EDVR-only GPU cost. Existing EDVR pass/door readings remain unchanged;
complete accounting of EDVR work across every early path is still separate
work. Subtracting those existing readings from this span is not a game-only
cost measurement.

The span measures elapsed GPU-stream time, including bubbles and contention. It
excludes commands after the final marker and commands issued before the pose
boundary. The preceding Frontier trace contains both kinds of work. It must not
be presented as the whole GPU frame or expected to equal SteamVR's scene/total
timing definitions.

## Placement and ownership

Covered base-context paths are direct/instanced/indirect draws and DrawAuto,
dispatch/indirect dispatch, RTV/DSV/UAV clears, copies and structure counts,
updates, resolves, mip generation, ExecuteCommandList, Map/Unmap and query
Begin/End. ClearState also consumes an armed boundary. EDVR's frame-query
commands bypass re-entry and the draw census. State-only setters are not a
universal start boundary. Extended-context upload/copy/clear variants and calls
that bypass the hooks are not proven coverage; the label deliberately says
render-to-submit from the first **covered** command.

The four new base-context slots are checked against the installed Windows SDK's
C vtable. They preserve the existing LiveCopy table contract and the separate
exposure hooks. The draft incorrectly assigned DrawAuto to slot 16; review
corrected it to slot 38 before execution. The paired test also verifies that
slot 16 still forwards PS constant-buffer bindings.

A version-1 paired export carries CPU pose/submit events. It changes no shared
memory layout. The graphics receiver allocates monotonic sequences across
compositor rebinds. Pose publication is atomic and issues no GPU commands; the
actual context pointer, device and calling OS thread gate the query ring. The
bridge resolves the exact executable-side graphics DLL and holds a module
reference for each callback, without loading a runtime or retaining a dangling
function pointer.

Eight frame slots hold six timestamp queries each. A frame owns one shared
frequency scope; existing pass timers borrow it. A live standalone interval
causes the frame measurement to skip rather than open an overlapping disjoint
scope. Queries are polled without flushing or waiting, retain partial ready
results, and expire after two seconds. An uncertain End permanently stops the
instrument until owner teardown. Query/COM cleanup is explicit; process exit
does not run it from destructors.

Source frames use the existing Present counter, captured when rendering starts,
not a second independently incremented frame counter. Results retain that
identity and their pose sequence even when they complete later. The Monitor
uses a locked CPU snapshot, rejects stale/invalid values, and never replaces a
newer sample with an older one. A late duplicate or foreign-thread event can
invalidate an already closed pair; affected older pending samples are discarded
conservatively rather than risk publishing a rejected frame as valid.

## Monitor and configuration

`advanced.app_gpu_timing = on` enables the local queries by default in this
checkpoint. It is live and independent of `advanced.compositor_timing` and
`advanced.openvr_census`. Turning it off closes/discards the local ring on its
verified owner; enabling it waits for a new pose boundary. If every hook-using
feature was disabled at startup and its hook was never installed, enabling this
feature requires a restart. Turning queries off retains the small
boundary-bridge and hook checks; an enabled/disabled comparison does not
measure that fixed cost.

The Monitor adds a render-to-submit line below the existing 16 tiles, including
D3D11 source, original frame and result age. Existing SteamVR GPU total, scene,
compositor, drops/reprojection, graphs and attribution are unchanged. Only the
small FPS overlay may use a fresh local fallback when compositor GPU timing is
absent, explicitly labeled `submit gpu`. Invalid/pending/stale results are not
zero-millisecond measurements.

## Validation

The focused suite passed 599 checks, including command-derived nested frame and
existing-pass timestamps, both eye orders, preserved source frames, partial
readiness, all seven allocation positions with failure/null-success variants,
disjoint/zero-frequency/unexpected GetData results, uncertain closure, ring
pressure, standalone-scope rejection, wrong thread/context/device, missing and
duplicate eyes, rejected submit, out-of-order pose waits, and configuration
disable/re-enable, immediate readout invalidation after failed poses, and
CPU-only frame publication from a foreign Present thread. Actual WARP
clear/copy workloads also verify every output pixel outside the measured
interval. Counts include repeated resource checks, not 599 distinct scenarios.

The paired build passed in `build/render-submit-validation-3.log`, including
the existing UI, motion, shader-bytecode, native adapter, shared-clock,
LiveCopy, OpenVR ABI, config and Python gates. Five isolated actual-proxy modes
pass: ready/delayed/disabled census and local GPU timing on/off. The local
modes have census and compositor timing disabled, use the two shipping proxies
with a fake historical compositor and real WARP, verify PS constant-buffer
bindings, and check every copied pixel. The first valid result in the separate
paired run was sequence 1 / source frame 1, 0.0113 ms outer and 0.0004 ms per
submit path, on context `000001DAF9C1F9A8`, thread `15324`. These software
timings are not performance targets.

The independent-feature test initially found that the compositor install gate
omitted local timing. The gate now includes it; the graphics hook install gate
also includes it. Neither census nor another rendering fix is required to start
the instrument. The final foreign-Present/failed-pose changes additionally
passed the 599-check focused suite. The final full paired build passed in
`build/render-submit-final-validation.log`, including all five actual-proxy
modes and the 599-check suite.

The headset run below did not qualify this span. WARP results establish the
tested command ordering and resource correctness, not hardware timing accuracy,
overhead or visual equivalence with production TAA/DLSS. The paired frame test
had temporal rendering disabled; the separate temporal smoke did not drive
OpenVR pose boundaries. Neither exercised temporal rendering with the outer
frame scope active.

## Frontier gate

Use Pimax through SteamVR on the Frontier install. Preserve the tuned INI.
Check prompt intro, cockpit and on-foot rendering/tracking, F8 Monitor close
and reopen, and normal exit. The new Monitor line should settle to a measured
value or explicitly explain unavailability. Compare both installed build
identities in the new logs before interpreting any readings.

Inspect owner/sequence/frame/age and valid/invalid summaries, active LiveCopy
hooks and existing pass/feature counters. This first run is a functional gate.
Matched-frame SteamVR correlation and a controlled enabled/disabled overhead
comparison remain required before treating the local value as qualified. Other
runtime/headset configurations and complete lifecycle/command coverage remain
separate Phase 0 gates.


## Installed checkpoint

Commit `0ac3095` (`v0.15.1-54-g0ac3095`) was rebuilt from a clean working tree.
`build/frontier-render-submit-clean-build.log` passed the full suite, including
599 shared/frame timing checks and all five actual-proxy modes. The isolated
production WARP smoke passed in `build/render-submit-clean-smoke.log`,
including black void, temporal history/cuts, supersample and sharpening checks.
Its log confirms the clean version, LiveCopy and a completed existing
shared-clock pass sample. NGX/foveation runtime checks remain unavailable on
WARP.

Both DLLs were installed and independently verified in Frontier at 08:22 local
on 2026-09-12. Steam was not changed and the game was not launched.

| Payload | Linked stamp | SHA-256 |
| --- | --- | --- |
| `d3d11.dll` | `6AA55F6D` | `168425BDAEE2DA1EF174A1069E3D539E100F5CD685F8072FC010E423ED969FAA` |
| `openvr_api.dll` | `6AA55F75` | `1DBDCB030D962E5E312393138DD54714BCCADAA0BB89897B54A9D19ED97A4D4B` |

The tuned root INI remained byte-identical, SHA-256
`A32D631834E5C1EEEFAA03367750A7A4211652869EB4976900DC11CF530EC1A9`. There was
no separate Openvr/win64 INI before or after installation. Previous DLLs were
retained with suffix `.pre-0ac3095-20260912-082225.bak`. The later
installation-record commit does not change that DLL version. The following
flight used `0ac3095`; the rollback below supersedes this installation.

## Head-movement regression and rollback

Renewed shimmering was reported during head movement after this flight. This is
a failed visual gate, even though the instrument produced valid samples. Do not
describe it as a proven motion-vector defect or a proven query side effect yet.

Both graphics and VR logs were independently verified against `0ac3095` with
the sanctioned log tool. The installer's verification mode also confirmed both
installed DLLs matched the checkpoint. Detailed flight evidence and the
rollback receipt are retained locally.

Observed evidence:

- LiveCopy is active. Periodic timing summaries report valid samples and no
  invalid samples on the expected owner. This refutes owner rejection or an
  inactive instrument as the explanation for this run; it does not establish
  visual correctness.
- DLSS, scene depth, camera rows and motion capture are active. Native TAA
  statistics cannot validate NVIDIA's history or the submitted motion vectors.
- Ruled out: a newly appearing projection-order warning, because both this
  verified run and the earlier normal `cc3d882` run contain it. The warning is
  an aggregate count, not proof of which projection each rendered eye consumed.
- Game CreateShaderResourceView failures are also present, with subsequent
  rendering observed. Their relationship to the reported shimmer is unknown;
  these are not evidence of failed timing queries.

Read-only review found no branch where timing lease failure or pending results
skip temporal rendering or motion capture. In `temporal_pass.cpp`, timing and
statistics completion control diagnostic-slot retirement; in
`celestial_motion.cpp`, the sampled timer result controls only its matching
End. The new context hooks forward the existing commands and the four added
slots match the SDK. This rules out those direct code couplings, not an
indirect scheduling or driver interaction.

The rollback did not restore visual quality. The next discrimination uses the
existing paired eye capture on that baseline: one still-head sample and one
sample during a slow head turn, in the same stationary cockpit scene with DLSS
enabled. Paired capture is already the default; no new build or configuration
edit is needed. The Instruments action "Dump both eyes as seen" collects the
raw and treated sequences before the later EDVR menu overlay, plus motion
metadata and first-frame shader inputs.

Check history flags and reset reasons, camera continuity, depth coverage and
the actual motion texture against displacement measured from raw frames. The
first-frame motion texture must be aligned to its capture frame; later frames
have metadata, not a separate captured motion texture. Compare input/output
crop coordinates at their recorded resolutions. These observations distinguish
history resets, incorrect reprojection and reconstruction blur; aggregate
projection counts cannot do so. No rendering-code fix has been made on the
current evidence.

The existing hardware smoke also passed using the restored graphics DLL and the
same DLSS runtime in an isolated fixture. NVIDIA's history accumulated across
evaluations, the motion probe ran successfully, and the shipped motion/jitter
pairing had the lowest error among the tested conventions. Those synthetic
results do not establish which projection or pose the game actually consumed.
They do not rule out game-driven resets or mismatched camera/depth inputs
during the reported head movement.

The exact retained baseline backups were hash-checked, staged separately from
the current build outputs, installed through `tools/install_edvr.py`, and
verified again. Current INI bytes were preserved. Use `--expect-build cc3d882`
for the next flight and the staged rollback root when verifying the current
installation. The subsequent headset comparison confirmed that the rollback did
not recover visual quality.

The capture investigation subsequently established a raw-only culling-probe
projection mismatch; see the [evidence and
correction](temporal-projection-probe-2026-09-12.md). The corrected `f622cd2`
pair, including the current timing work, has replaced the rollback in Frontier.
Both DLL hashes match the clean build and the current INI bytes are preserved.
Use `--expect-build f622cd2` for the next flight. Visual recovery remains the
gate before further OpenXR implementation.
