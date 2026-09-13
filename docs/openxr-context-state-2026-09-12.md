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
added and ran the actual-DLL shadow fixture. No native headset result is
claimed for this executable yet.

The updated PiOpenXR executable requires a fresh headset readiness gate after
desktop review and a pushed checkpoint. The previous skybox receipt qualifies
the earlier executable only.

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
