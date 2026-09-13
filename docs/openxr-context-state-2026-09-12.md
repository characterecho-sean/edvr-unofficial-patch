# OpenXR renderer context preservation

This checkpoint follows the passed six-face loading test. It prepares the
renderer to borrow Frontier's D3D11 device by preserving the immediate context
and the graphics proxy's binding shadow. It does not yet connect the native
transport to Frontier or establish safe ownership between the game render
thread and the OpenXR owner. The installed Frontier pair remains `f3c205e`.

## Rendering contract

`D3D11Stereo` creates a private deferred context on the exact supplied device.
Triangle, diagnostic eye, captured-eye and skybox draws record a complete pass
for each eye. `FinishCommandList(FALSE)` resets the recording state, so the
next eye binds its complete pipeline again. The immediate context executes each
list with `ExecuteCommandList(TRUE)`, then flushes before the associated OpenXR
image is released. The diagnostic eye path executes before returning its
borrowed texture for capture.

Microsoft documents that [ExecuteCommandList with
restoration](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-executecommandlist)
saves and restores the target context's state. Command lists [do not inherit
prior
state](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-finishcommandlist).
This avoids a hand-maintained subset of pipeline bindings and avoids sending
private shader and resource setters through the game's immediate-context
feature hooks.

The existing graphics proxy already retains its binding shadow when
`ExecuteCommandList` restores context state. Its normal execution hook still
observes the submission and conservatively invalidates resource-content
consumers. Private deferred commands do not become game draw calls for shader
fixes. No context vtable is bypassed or assumed to be an unhooked runtime
table.

Failure to create a deferred context rejects initialization before swapchain
calls. In particular, Microsoft [does not allow deferred contexts on
SINGLETHREADED
devices](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createdeferredcontext).
The implementation does not change device flags, toggle multithread protection,
or substitute another device.

Only a successful image wait permits recording, execution and release. A
timeout or uncertain runtime operation retires the renderer. A failed
command-list finish releases the recording context; `ClearState` would merely
append another recorded command and would not discard that work. Shutdown
releases the deferred context before destroying swapchains and never clears or
flushes the caller's immediate context.

## Validation

The full absolute-path `build.bat --openvr <Frontier original DLL>` passed. The
stereo suite passed 10,774 checks, including the original 5,240 triangle, crop,
color and skybox checks. Counts include repeated fixture assertions, not
independent scenarios. Added state checks cover all six shader stages,
high-slot constant buffers, SRVs and samplers, IA bindings,
rasterizer/viewport/scissor, target/depth/blend state, a false caller predicate
and a UAV's hidden counter. The caller's target pixels stay unchanged. Direct
triangle output is checked with that predicate active to catch accidental
inheritance.

Each composition path is exercised at both eyes' acquire/wait/release failures,
positive wait timeouts and invalid geometry. A failed renderer cannot replay
work on retry, and cleanup preserves the caller state. A SINGLETHREADED device
must reach deferred-context rejection with valid view dimensions and no XR
calls; rejection due to an unrelated validation error cannot pass that check.

The actual built graphics proxy passed its separate 196-check WARP integration
fixture. A read-only build-test export reports the DLL's binding-shadow
identities and generations; the fixture verifies all 14 shadow slots, unchanged
game target pixels and all four renderer paths. It requires the real
ExecuteCommandList hook to run and no immediate ClearState hook to run before
cleanup. Stream output, D3D11.1 constant-buffer ranges and vendor extension
state are not independently exercised by these sentinels.

Other gates include 476 binding, 22 published-device, 150 frame/loading, 74
skybox-copy, 62 owner-service, 219 compositor and 28 native harness desktop
checks. The configuration contract remains 252 keys. The complete build log is
`build/openxr-context-state-build.log`; executable and source hashes are saved
in `build/openxr-context-state-validation.json`.

Luna supplied the renderer and initial state tests. Parent review corrected
recorded-command retirement and cleanup order, resource hazards and API types
in the test fixtures, an unrelated-validation false positive in the device
rejection test, and missing failure/predicate/counter coverage. Parent also
added and ran the actual-DLL shadow fixture. The subsequent native run is
recorded below.

The updated executable completed the native run below after fresh headset
readiness. Sean confirmed that the grid, transition, triangle tracking and
closure all looked normal. The previous skybox receipt qualifies the earlier
executable only.

## Native run of `8ff82f2`

After Sean confirmed readiness, preflight verified all 80 recorded source
hashes, the native executable, the checkpoint and the loader/manifest hashes.
Frontier and SteamVR were absent. The Python runner selected PiOpenXR only in
the diagnostic child's environment; no installed DLL, saved runtime or live
configuration was changed.

`build/openxr-native-20260912-202150` contains the output, receipt and post-run
process snapshot. The run started at `2026-09-13T02:21:50.794848Z` and ended at
`02:22:11.198147Z`, after 20.406 seconds, with exit 0 and no watchdog timeout.
Pimax OpenXR 0.1.0 used D3D11.1, two 5424x5356 eyes and swapchain format 29
(RGBA sRGB). The executable SHA-256 was
`9006ffd2f9bcbf3e4dbf139ee28bc667190432e3240d405c48b25878f406a90c`.

The native checks passed:

- Startup published valid geometry on frame 1, after one zero-layer frame and
  before any scene Submit. Repeated Init retained identity and token 1; the
  separate caller read cached geometry before Compositor discovery.
- The loading override produced 234 projection frames with one startup game
  wait, zero game Submits and unchanged game pose caches. It transitioned once
  to scene rendering, then cleared and retired its private textures.
- The main loop completed 1,532 waits, 3,058 copied-eye Submits, 1,529 stereo
  pairs and 1,531 cache comparisons. All 1,530 reported view/head samples were
  valid. The summary reported 1,531 main-loop frames and two empty frames;
  loading frames have separate counters.
- Both recenter operations and reset events passed. Init, System and render
  owner ran on distinct threads (30820, 39912 and 2932). The service reported
  1,810 event pumps and 1,839 valid live System queries out of 1,840; the cause
  of the one invalid query was not established by these counters.
- System-thread Shutdown joined the owner, retired interfaces, advanced the
  token to 2 and reported normal stop and cleanup. The post-run snapshot at
  `02:22:30.4201578Z` also contained no Frontier or SteamVR processes.

This exercises command-list playback on the native headset renderer. The
explicit game-state and binding-shadow sentinels remain desktop tests. Sean
answered "Yes—all looked normal" to the check for grid placement in both eyes,
upright and fixed tracking during head turns, normal transition, triangle
appearance/tracking and normal closure. This passes the bounded headset gate;
it is not a native Frontier flight or a performance comparison.

## Remaining integration

Restoring state does not serialize the immediate context. Microsoft requires
[serialized immediate-context
execution](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-render);
the standalone diagnostic still owns all such work on its service thread.
Frontier's rendering must have an explicit exclusion and ordering contract
before the native owner can copy or execute on its device, especially during
autonomous loading frames and shutdown. A lock used only by the OpenXR worker
cannot establish that contract.

The graphics device publication also needs a compatible paired-module
capability and lifetime contract before native shipping discovery. Full legacy
exports, game feature integration, native startup/flight/exit, device loss and
focus/lifecycle qualification remain open. These desktop tests do not establish
vendor extension state preservation, driver overhead, or Frontier
temporal-rendering parity.
