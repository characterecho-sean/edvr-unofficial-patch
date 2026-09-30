# Build focus steal: a full build took the keyboard (2026-09-29)

During a full `build.bat`, some test exes took the keyboard focus from
whatever window Sean was typing in. Caught in the act with a monitor, fixed at
the source, and now guarded by the build itself.

## Status

State: FIXED and GUARDED on branch `claude/build-no-focus-steal`. Not merged,
not installed, not flown.

Cause, measured: three rigs (`openxr_present_test`, `openxr_shutdown_test`,
`openxr_module_test`) create a swap chain on a hidden top-level window
(`PresentDevice`, `tools\openxr_native_test\present_device.h`). The proxy they
load hooks the swap chain and force-foregrounds its window (`hookSwapChain` ->
`forceWindowForeground`, `src\d3d11\device_hook.cpp`; key
`d3d11.focus_on_launch`, default on, meant for the game's own window). A hidden
window takes the foreground as readily as a visible one: 11 moves in one build.

Fix, in two parts, both needed: the proxy takes the foreground only for a
top-level window on the desktop (`src\d3d11\focus_target.h`; the game's is one,
so the game is unchanged), and `PresentDevice` makes its window a child of a
message-only one. The child alone leaked: in one build the transport rig's five
launches each took the foreground for the child's ancestor. The proxy now does
not attempt the move for such a window at all; the rig logs say `focus-on-launch
skipped`. `openxr_present_test` checks the rule and its own fixture against it.

Guard: `tools\run_jobs.py` watches every job with `tools\focus_watch.py` and
fails the build if a process of a rig shows a window, opens a console or takes
the foreground. Its log line `focus_watch: no window was shown ...` says the
guard looked. `focus_watch.py --self-test` gates build.bat's head. The runner
also sets its error mode (no crash, assert or missing-DLL dialog), inherited by
every job.

Ruled out (each with the evidence):

- ruled out: console windows from `CREATE_NO_WINDOW` parents (hypothesis a),
  because a child of such a parent inherits its windowless console; only a
  parent with no console at all (`DETACHED_PROCESS`, a GUI parent) makes Windows
  open a terminal, and a full build showed 0 console windows.
- ruled out: GUI windows from the installer or settings view in a gate (b),
  because a full build showed 0 visible top-level windows.
- ruled out: crash and assert dialogs (c) as a cause, because a full build showed
  none, and a native access violation and an `abort()` under `CREATE_NO_WINDOW`
  opened no window here even with the default error mode (WER ran ~1.1 s
  instead of ~0.1 s). The error mode stays: it is what makes a rig fail in its
  log everywhere else.
- ruled out: making the fixture window disabled, `WS_EX_NOACTIVATE`, a tool
  window or message-only, because the proxy's own steps foregrounded every one
  of them; only a `WS_CHILD` window refused (probe below).
- ruled out: a visibility test in the proxy (`IsWindowVisible`), because nothing
  shows that Elite's window is visible when its swap chain is created, and that
  would cost a flight. The top-level test cannot differ for the game.
- ruled out: job-object UI limits (`JOB_OBJECT_UILIMIT_HANDLES`), which would
  blind the proxy's `GetForegroundWindow`: broad side effects on compilers and
  DXGI, and it hides the cause instead of fixing it.

Next: nothing for this arc. If a build fails on `focus guard`, the list above the
ERROR names the exe, the rig and the chain of parents. To find one again:
`python tools\focus_watch.py --run --log build\focus.jsonl -- cmd.exe /d /c build.bat`.
The proxy change alters the graphics DLL: it is a no-op for a top-level window
(the game's) and has not been flown.

## Journal

### Baseline: caught in the act

A full build of 905712e1 with `focus_watch.py --run` attached (a
SetWinEventHook plus a 50 ms poll; each finding names the exe, the chain of
parents and the rig from `--rig <label>` on its cmd.exe): 213 s, exit 0, 11
foreground moves in 3 groups, every one the 64x64 `EDVR hidden Present fixture`:

    openxr_present_test.exe   x6   first at +147.0 s   job openxr_present_test
    openxr_shutdown_test.exe  x1   first at +147.1 s   job openxr_shutdown_test
    openxr_module_test.exe    x4   first at +152.1 s   job openxr_module_test

Hooks saw 34 show, 0 console and 29 foreground events (the rest were other
apps taking the foreground back as each fixture died). Those rigs' logs said
`window: focus-on-launch applied`. `openxr_native_test` uses the same fixture but
the build only runs its `--dry-run` and `--self-test`, so it did not show.

### The probe: what the proxy's steps can foreground

The proxy's exact steps (attach to the foreground thread, `BringWindowToTop`,
`SetForegroundWindow`) against each kind of hidden window:

    hidden popup, disabled popup, message-only popup, tool window+NOACTIVATE   foreground
    WS_CHILD of a message-only window                                          refused

Same harness, three rigs plus the transport matrix, monitor attached: the old
fixture 11 findings in 3 groups (6, 1, 4: the same counts as the full build), the
child fixture 0.

### The child alone was not enough

The first two full builds with the child fixture: one clean, one not. The second
had 10 foreground findings, all `openxr_present_test.exe`, all from
`tools\test_openxr_transport.py` (five launches, each the child and its parent,
+80.0 to +82.5 s). Its private `edvr.ini` disables logging, so the proxy's own
line is not there to read. It did not reproduce in 60 further launches with Sean
at the keyboard (the desktop's whole foreground sequence recorded: no fixture
window in it), and the state it needed is unknown. Whatever it is, the fixture
cannot control it, so the proxy stopped attempting the move. `PresentDevice`
stays a child so the rule has something to skip.

### After

Final full build on the source as committed, `focus_watch.py --run` around all
of `build.bat`: exit 0, all gates passed, receipt `full-pass` (inputs
2e491f48...). The outside monitor: 0 findings in 150 s (the desktop's hooks saw
2 show, 0 console, 0 foreground events). The runner's own guard: `no window was
shown ... in 109 s`, the pool 92.8 s (87.9 to 103.7 s in the other three builds,
which is machine load, not the guard). The rigs' logs: `focus-on-launch skipped`
for the fixture windows of `openxr_module_test` (8), `openxr_present_test` (2)
and `openxr_shutdown_test` (2). The build before it, differing only in a comment
and two test timing margins, was as clean: 0 findings in 145 s.

### The guard, and why it is a monitor

The offender was production code applied to a fixture window, not a spawn flag,
so a check on the spawn helpers would not have seen it; only watching the
desktop does. It runs inside the runner because every rig, and so every exe that
loads a production DLL or makes a window, runs under it. Not covered: the build's
serial head and tail (compilers, self-tests, packaging); an outside monitor over
the whole build found nothing there either. Cost: 0.55% of one core over 20 idle
seconds (a poll that inspects only windows it has not seen). A self-test window
titled `focus_watch probe` (1x1, off the desktop, never activated) proves the
watch sees a window; a watch not under test ignores that title, so a build watched
from outside shows none of them.

Mutation check of the guard's own tests: 12 changes to `run_jobs.py` and
`focus_watch.py` (guard never fails the run, never started, main forgets it,
import error swallowed, error mode not set, report not printed, watch records
nothing, rig label unreadable, tree root ignored, probes always counted, always
ignored, the build's guard counting them), each killed by a self-test.
