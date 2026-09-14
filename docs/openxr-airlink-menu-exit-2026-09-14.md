# Air Link menu and exit investigation

The 2026-09-14 Frontier flight loaded the native backend through Meta's
Windows-selected OpenXR runtime, but Elite's main-menu items were invisible.
Sean could select the invisible Exit item; the desktop window disappeared while
EliteDangerous64.exe (PID 25644) kept running. This is a failed flight, not
feature-parity qualification.

## Identified build and environment

The installed pair matches the archived qualification for
`v0.16.2-94-g663b49d-dirty`, reviewed in commit `12152c5`. Native runtime
SHA-256 is `77ce22d8bd544fb3e96e3396ace8a76cf5f3dedc459af13ea5ac795e4e9be804`;
graphics SHA-256 is
`d377ede3012560c0884314b7f51e12c86b58d6e01a925f619385c5592a3a8d70`. Native
startup configuration still selects `runtime=system`. The native trace names
the runtime Oculus. This is the Quest/Air Link route under investigation.

Startup captured a 100% render scale and a runtime recommendation of 1824x1968
per eye. The game used DLSS. During the flight the user changed the saved scale
through F8, ending at 180%; that value is pending a restart. The culling guard
separately adopted a 1.24251 horizontal factor, recommending 2266x1968 for the
game's eye buffers. Preserve the live INI during subsequent installations.

## Evidence retained

Private artifacts are under `build/airlink-blank-menu-20260914`. Flight logs
were read and archived through `edvr_log.read_text`:
`edvr_gfx_20260914_101035.log` and `edvr_openxr_20260914_101036_329_25644.log`.
Local time is UTC minus six hours.

- At 10:10:36 the route reports profile recognition, two exact legacy-library
  rejections from audited caller RVA `0x4E70BC`, and zero failures. Native
  startup succeeds with geometry ready. Ruled out: stale DLLs or the previous
  ASLR profile-recognition failure, because artifact hashes and successful
  route counters agree.
- Native startup captures scale 1.0 before eye allocation. Ruled out: the saved
  180% value immediately resizing this session, because startup captured 100%
  and the new setting is applied only at initialization.
- At 10:10:58 a seated reset invalidates geometry; immediate projection queries
  report invalid sequence zero. Normal native timing samples continue afterward
  through approximately 10:11:30. Ruled out: that reset permanently stopping
  headset submissions, because later valid timing samples exist.
- The 10:11:28 eye dump's input `C00`, treated `T00`, and full left eye `L0`
  all show the hangar floor and ship belly/landing gear, without Elite menu
  text. The original BMPs and lossless PNG conversions are retained. Ruled out:
  DLSS alone erasing otherwise visible menu text, because it is also absent
  from the input capture. This does not yet establish whether the UI draw was
  skipped, transformed outside the view, or composed elsewhere.
- At 10:11:35 projection, raw-FOV, eye-transform and recommendation queries
  report invalid geometry sequence zero. The native log ends there without
  shutdown-stage messages. Graphics logging continues at approximately 800
  frames per second, with no native timing samples and later nonfinite viewer
  values.
- The window handle is zero after the user's Exit selection. A Windows PSS
  snapshot captures the remaining threads without terminating the game.
  `snapshot-elite-1.txt` records a native owner waiting on a condition
  variable, not inside `xrWaitFrame`; the game render thread is sampled inside
  D3D11/NVIDIA work, and the main thread is in its message wait. Snapshot
  enumeration and cleanup both succeed. Names with large displacements are
  nearest exported symbols, not reliable private function names.

## Discriminating checks

1. Missing UI: inspect the saved GUI and eye draw snapshots for UI draw
   presence, finite camera constants, target identities and projection
   placement. Compare native projection/eye-transform availability at the
   game's initialization and reset queries. A clear hangar alone cannot
   identify the missing UI stage.
2. Invalid geometry: determine whether the live native session has retired,
   stopped, lost tracking, or rejected a geometry sample. Returning zero
   projection matrices can contaminate a game's cached camera, but a proposed
   correction needs the causal state and regression evidence.
3. Exit: resolve the native owner and game wait stacks and check whether
   shutdown was entered. An idle owner contradicts an explanation based on an
   owner stuck inside `xrWaitFrame` at the time of that snapshot. Do not change
   shutdown ownership or terminate the process based only on missing summary
   logs.

## Confirmed session lifecycle defect

The live process state was read without injecting code or calling the runtime.
The read is tied to the verified native DLL hash, the module-global load
instruction in `edvrGetNativeRuntimeStatus` at RVA `0x25061` (global RVA
`0x7d648`), and MSVC class-layout output from the unchanged source. Thread IDs
and the versioned status header match the flight. Raw records and decoded
fields are retained in `live-native-state.json`, `live-module.bin`,
`live-generation.bin` and `live-host.bin` under the private artifact directory.

The native module still reports Running, with shutdown thread zero and zero
host stop calls. The host reports `serviceStopped=1`, `serviceFailed=0`,
session lifecycle Ended, OpenXR state STOPPING, `running=0`, `terminal=0`, no
hard failure and a successful last result. It completed 3735 waits and 7464 eye
submissions with zero pose failures. This is a successful `xrEndSession`, not a
blocked call or an uncertain failure.

`pumpEvents()` returns immediately whenever `serviceStopped` is set. Its
STOPPING handler sets that flag after ending the session. Consequently, it can
never consume a subsequent EXITING or IDLE/READY event. The OpenVR facade
remains published, but the session cannot resume or notify Elite of terminal
runtime exit. Ruled out: a blocked `xrWaitFrame`, GPU shutdown fence or native
owner join as the observed stop, because the owner is idle, shutdown has not
started, and the session has already ended successfully.

The correction must keep polling after STOPPING, resume only when the runtime
permits it, preserve coherent publication sequence ordering, and expose genuine
terminal runtime exit through the game-facing event contract. STOPPING alone
must not be treated as a request to quit the game. Teardown must remain outside
the owner idle callback to avoid joining that same thread.

Sean cannot yet establish whether the menu was missing at startup; it may have
disappeared after editing resolution. The saved percentage remained pending
while the culling guard and seated reset changed publication availability.
Captured UI constants are being checked before attributing the missing menu to
that transition.

The diagnostic evidence was saved before the game process ended. The corrected
test installation is recorded below.

## Calibration availability correction

The recorded recenter and ended-session queries expose a separate API defect:
the projection and eye-transform getters return all-zero matrices when only
frame tracking has been invalidated. These OpenVR getters provide no
pose-validity flag for their callers. An all-zero projection or rigid transform
is unusable for a cached game camera. The correction keeps validated,
unjittered game-facing FOV and eye-to-head calibration for the current session
generation, independently of frame pose validity. Current valid frames still
use their live temporal shift; invalid frames do not reuse that jitter or any
world-space head pose. Retirement clears calibration.

The cached calibration must retain the adopted culling frustum consistently
with the game's recommendation. This does not resize the active OpenXR
swapchains or apply a pending resolution percentage. Regression checks must
cover queries immediately after recenter/invalidation, tracking recovery,
rejected geometry, jitter removal and generation retirement.

This correction is not yet a proven explanation of the missing menu. The saved
panel-family camera constants inspected so far are finite and nonzero, and
those shaders also occur outside menu composites. Do not claim that the dump
contains a zero UI matrix or that an unseen menu is fixed without the relevant
draw evidence or the next flight.

The eye-dump draw census narrows the visible failure further. At 10:11:28.475,
`DC 0 #661/#664` uses the panel VS `A888D51024D9798E` and composite PS
`9107E72CB016CC02`; the sampled-resource list is `tex512x512f70,-,-,-` (or a
1x1 texture in slot zero). PS slot one, from which this composite reads the
interface surface, is unbound. The same missing binding occurs in both eyes and
the following two census frames. This agrees with the snapshot's lack of
captured interface surfaces. It establishes a missing UI input at the
composite, rather than text lost in DLSS; it does not yet distinguish an absent
game UI resource from a lost binding during reconfiguration.

The EDVR F8 menu closing at 10:11:15 does not invalidate this capture: Sean's
report concerns Elite's main menu, not EDVR's menu. EDVR menu-provider/footer
measurements must not be used as evidence that Elite's menu rendered correctly.
On the next flight, inspect Elite menu visibility before changing settings,
after a saved resolution change, and after recentering. If it disappears, take
an eye dump immediately; the existing census can again distinguish a missing
interface resource from bad placement.

The original-draw ordering was verified in `beginPanelOverride`: the census and
object snapshot occur before forwarding Elite's draw. The later UI-depth
reissue modifies PS slots 2 and 14, not slot 1, and bypasses these capture
hooks. The missing binding therefore belongs to the original composite, not the
temporary depth pass. The snapshot reads actual immediate-context SRVs. No
configuration-reload path that clears slot 1 was found; the temporary draw
treatments that use that slot save and restore it. This rules out the depth
reissue as the source of this particular capture, but does not identify why
Elite's interface input was absent.

## Implemented corrections and verification

The host now keeps polling after a successful STOPPING/end transition. READY
can restart the same session with its existing publications and graphics
providers; outward frame sequences continue above their previous sequence
floors while the internal OpenXR frame token remains unchanged. Restart clears
transient pose, menu, feature, temporal and timing state. Terminal events or
failed end/restart operations queue an OpenVR Quit event for Elite; STOPPING
alone does not. Lifecycle changes, cached optics availability and shutdown
entry are logged. Cleanup still belongs to the existing shutdown coordinator.

System queries use valid live geometry when available and otherwise use the
generation's validated unjittered optics. Fallback queries do not advertise a
stale projection sequence to temporal processing. Retirement and a fresh
generation clear the cache.

The absolute-path full `build.bat` passed with all 537 source hashes unchanged,
including 554 session checks, 123 native host checks, 157 system checks, the
five-case transport matrix, native module/bootstrap tests, Oculus routing and
the 255-key configuration contract. Separate tests against the built graphics
DLL passed actual TAA and DLSS processing (169 checks and 18 eyes each), plus
sharpening enabled (58 checks) and disabled (22 checks). Regression fixtures
exercise real frame boundaries across two resumes, retain optics while
invalidating poses, preserve sequence ordering, and expose terminal Quit once.

The verified pair `v0.16.2-95-g12152c5-dirty` was installed into Frontier
through `tools/install_edvr.py`, after its dry-run preview. Runtime SHA-256 is
`f8c70eca4d7a320a76dacf5342d5c13e9284c007573843d074ef84582c5dde5b`; graphics
SHA-256 is `bef1c2be178d21cbb9f0322a4a7fe0338a853b1ac5be2b3f3455fccc74a845a3`.
Hash verification passed. The live `edvr.ini`, including the saved 180% scale,
and original OpenVR DLL were preserved; runtime selection remains `system`. The
private qualification archive is `build/openxr-airlink-menu-exit-20260914`. No
game was launched by the agent.

## Air Link retest checklist

1. Check Elite's main-menu text in both eyes before changing anything. The
   saved 180% render scale will apply at startup.
2. Change the saved OpenXR resolution percentage in F8, then inspect Elite's
   menu again. Recenter and check once more. The percentage remains pending
   until restart; it should not resize the current session.
3. If Elite's menu disappears, press Insert immediately and note which action
   preceded it. Compare the desktop mirror if available.
4. Select Elite's own Quit option and verify that the process exits normally.

## Successful Air Link retest

Sean reported "Worked perfectly this time" after the checklist above. Both
flight logs identify the installed `v0.16.2-95-g12152c5-dirty` pair reviewed in
`5b9dc99`; the installer also verified the current pair and configuration. The
Windows-selected runtime is Oculus. Startup captured 180% resolution, 3283x3542
per eye from the runtime's 1824x1968 recommendation. The culling guard later
recommended 4079x3542 game buffers. DLSS engaged in both eyes.

The native trace records seated resets at 16:59:10.805 and 16:59:13.264 UTC.
Immediate game queries correctly retain `optics_valid=1` at sequences 1857 and
2018 while frame geometry is invalid. The run completes 13,918 stereo pairs
with zero pose, temporal-treatment or wrong-thread failures. Graphics temporal
totals report 27,811 treated eyes, zero missing projections and no stand-down.
Resolution percentage edits are not separately present in these logs; the
user's success report supplies the visual result of the requested retest.

Elite enters native shutdown at 17:02:00.892 UTC and finishes at 17:02:01.369
UTC. Every recorded teardown stage succeeds; the final module summary reports
`owner_joined=1,cleanup=1,callback_retired=1,retained=0`. The game process is
no longer running. This qualifies the reported menu retest, recenter behavior
and normal Air Link game exit for this pair.

The two logs are archived under
`build/openxr-airlink-menu-exit-20260914/airlink-retest-105838`, with hashes
and the user's report in the qualification record. STOPPING/resume and terminal
Quit event delivery were not exercised by this successful flight; their
injected-dispatch regression coverage remains distinct. The successful menu
result does not isolate which correction resolved the previously absent
interface input. Cross-runtime feature qualification and release installer
migration remain separate work.
