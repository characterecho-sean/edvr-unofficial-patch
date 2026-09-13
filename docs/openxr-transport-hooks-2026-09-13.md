# Native graphics transport with optional vScreen hooks disabled

The [native bootstrap checkpoint](openxr-native-bootstrap-2026-09-13.md) passed
its Pimax headset gate. Graphics discovery still depended on vScreen installing
its ExecuteCommandList hook. When the optional-feature gate deliberately
skipped vScreen, the native backend could not discover a usable graphics owner
even though the game had created its device.

## Installation contract

The deliberate vScreen skip now installs a minimal ExecuteCommandList transport
on the same immediate context. It uses the already selected shared,
private-copy or LiveCopy mechanism. Its single hook consumes the paired
module's one-shot private permit, counts unpermitted owner calls, and forwards
the command exactly once. Other contexts sharing the table are forwarded
without altering the registered owner's counters. Graphics and Present owner
registration follow successful hook commit; native Init still waits for an
actual Present callback before requesting graphics work.

The new path does not create vScreen state, configure its optional rendering
passes or bind its GPU timing. The normal vScreen path continues to preserve
binding shadows and invalidate resource history for unknown lists. The
pre-existing exposure and device hooks remain separate. Deliberate context
probes and crash-recovery suppression continue to leave native discovery
unavailable. A failed vScreen installation does not trigger a second
transport-hook attempt, but keeps its existing full-hook retry behavior.
Selecting the minimal route permits only one attempt, including failure; it
does not switch back to optional hooks afterward.

The forwarding target is initialized before atomic publication of the hook
state, which precedes commit. Hook state and retained device/context references
survive for the graphics module's lifetime. Teardown closes graphics
acquisition before removing the minimal hook. Private execution retains the
existing device, thread and restore-state restrictions, and a bypassed hook
reports unavailable after flushing potentially submitted work without retrying
it. No new reclaim or runtime fallback policy is introduced.

Published forwarding state remains available after a failed commit or
uninstall: an in-flight caller that already reached the thunk still forwards
when accounting is inactive. Clearing the published pointer would instead drop
that caller's command. Installation remains on the existing first-device
startup path, outside native Init. Forced hook-install failures, concurrent
competing installers and removal racing a game command are not independently
exercised by this checkpoint.

## Desktop evidence

Luna implemented the transport path. Parent review checked hook publication,
first-owner admission and teardown, corrected an overly broad retry guard, and
supplied actual-DLL regression checks. The fixture verifies native discovery,
Present registration and callback-held shutdown while confirming that vScreen's
ClearState and ExecuteCommandList hooks never ran. It checks private GPU output
and preservation of the game's render target, exact private/unknown counters,
permit non-reuse, foreign-thread rejection and rejection of another device's
command list on the same adapter.

`tools/test_openxr_transport.py` stages the built graphics DLL with an isolated
INI for each child. Three cases exercise shared, private-copy and LiveCopy with
optional vScreen hooks disabled; two cases verify that the deliberate swap/live
context probes keep native graphics and callback acquisition unavailable. The
fixture disables camera observation with `advanced.camera_buffer_bytes = 0`:
turning `fix.transition_flash` off alone still records history and therefore
still requests vScreen hooks. Temporary files are removed after each child
exits. Its self-tests cover a no-write dry run, child failure and timeout
cleanup.

The absolute-path full `build.bat` completed with exit 0. The normal Present
fixture passed 168 checks. Each of the three minimal-transport cases passed 184
checks; each deliberate-probe case passed 25 checks. These are repeated
contract checks across hook mechanisms, not independent scenarios. The runner's
three self-tests passed, as did the existing 169 explicit-module checks, 175
bootstrap checks, 67 queue checks, 100 render-dispatcher checks, 504
proxy-state checks, 10,774 stereo checks and 254-key configuration contract.
The existing graphics and motion suites also passed.

The same new fixture was then run against the archived `eee7a1c` graphics DLL,
SHA256 `fdbce5cf3b248665555f78f4cf6c761d1451c8edaa108b81d8ab8c21111d7e30`. It
exited 1 with exactly the two expected failures: no owner execution accounting
and no graphics bridge when optional vScreen hooks were disabled. This
establishes that the fixture detects the original missing transport, rather
than merely accepting a new flag.

The full build and regression output are
`build/openxr-transport-hooks-build.log` and
`build/openxr-transport-hooks-regression.log`. The record in
`build/openxr-transport-hooks-validation.json` contains 481 hashes: 464 source
inputs, 11 binaries, two logs, the explicit loader and runtime manifest, and
the two staged diagnostic files. The graphics DLL SHA256 is
`81c2bdd6b4e28b127d2304993ccd405404bc45b5a92592db5a29bbc79030e39a`; the native
module is `6da3994b047d67b7201eb3b3b1a21844e453de64ef2ace8d9e1ac5fbf6884242`,
and the module fixture is
`d28fd8c84ff37b95074254d73ef6c9e017f227644c644d05fd35eff1ee69dc06`.

## Next native gate

After fresh user readiness, run the existing 20-second bootstrap diagnostic
with the graphics DLL in `build/openxr-transport-native-stage-20260913-2/`. Its
isolated INI uses the desktop LiveCopy case's optional-off settings, with
diagnostic logging enabled. Require the minimal-transport installation marker
in that stage's graphics log, the expected configuration and startup records,
valid stereo submission, normal grid/triangle viewing and complete
callback-held cleanup. The runner's existing child-only runtime override and
watchdog remain unchanged. This stage does not modify Frontier's files or
settings.

## Remaining qualification

This checkpoint adds desktop transport coverage. The preceding headset flight
used the normal vScreen path and does not qualify the new minimal hook on the
Pimax driver. Complete legacy export policy, Frontier Init/Shutdown
render-progress evidence, game feature integration and a native Frontier flight
remain open. No game installation or persistent settings change is part of this
checkpoint.
