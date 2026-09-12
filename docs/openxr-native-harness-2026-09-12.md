# Native OpenXR stereo diagnostic

This checkpoint adds a standalone native-session harness. It loads an
explicitly supplied installed OpenXR loader, selects the runtime through the
loader's normal rules, creates a D3D11 device on the runtime's required
adapter, obtains native tracking and submits a stereo diagnostic triangle. It
does not replace Frontier's OpenVR DLL or use Elite's graphics device. The
subsequent [first PiOpenXR run](openxr-native-pimax-2026-09-12.md) passed the
native session/submission/shutdown checks and the user saw the diagnostic
triangle.

The Luna drafts were revised during parent review: frame submission,
inverse-pose projection, swapchain ownership, timeout handling, device
validation, partial cleanup, and tests required correction or completion. The
shipping proxies remain unchanged.

## Runtime and resource contract

`tools/openxr_native_test/openxr_native_test.cpp` requests OpenXR 1.0 with
`XR_KHR_D3D11_enable`, primary stereo and an advertised opaque blend mode. It
queries graphics requirements before session creation, selects the exact
adapter LUID, filters D3D feature levels against the requirement and validates
the created device again. It never falls back to a different adapter or to
WARP. WARP is used only by the separate desktop tests.

The harness owns one diagnostic device and immediate context, one
instance/session, LOCAL and VIEW reference spaces, and two single-sample eye
swapchains. It uses the runtime's recommended eye sizes and chooses a supported
RGBA/BGRA sRGB format when available. The explicit UNORM fallback encodes
shader output to sRGB. Compatible typeless runtime resources use typed RTVs.
Every runtime image is validated against the device, dimensions,
array/mip/sample layout and format family; acquired indices select among all
enumerated images.

The existing `SessionState` controls wait/begin/end and READY/STOPPING
transitions. Located eye views use the frame's predicted display time in LOCAL
space. A separate `xrLocateSpace(VIEW, LOCAL, time)` supplies the HMD pose; it
is not derived by averaging eyes. The first valid eye/head geometry is reported
even if `shouldRender` is false. Orientation and position validity are checked
independently of tracked flags, and invalid/nonfinite/nonunit geometry is
rejected rather than repaired. Native eye poses and FOVs are preserved in the
projection layer.

`src/openxr/d3d11_stereo.*` renders a stationary RGB triangle at two metres in
LOCAL space on an opaque black background. Its context-state ownership is
exclusive to this standalone diagnostic. It is not a drop-in rendering pass for
the game. The world-to-eye transform inverts the complete
quaternion/translation, and the projection preserves horizontal and vertical
asymmetry.

Each eye follows acquire, successful wait, draw, flush and release. A timeout
is a positive OpenXR status but does not permit drawing or releasing the image.
Timeout, loss and other failures retire the renderer; an incomplete pair
produces no projection layer and operations are not retried on uncertain state.
The owner tears down the affected resources. Borrowed runtime texture pointers
are not released as owned resources. RTVs and context bindings are released
before swapchain destruction. Cleanup reports the first error while attempting
the remaining cleanup.

These choices follow the Khronos [D3D11
binding](https://registry.khronos.org/OpenXR/specs/1.0/man/html/XrGraphicsBindingD3D11KHR.html),
[image
wait](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrWaitSwapchainImage.html),
[image
release](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrReleaseSwapchainImage.html),
[view
location](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrLocateViews.html),
and [space
location](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrLocateSpace.html)
contracts.

## Running a native test

Build with the ordinary absolute-path `build.bat`. The build runs desktop
fixtures only. A real runtime is used only by an explicit harness invocation.
For example, from the repository directory:

```powershell
python tools/run_openxr_native.py --loader 'C:\path\to\openxr_loader.dll' --runtime 'C:\path\to\runtime.json' --seconds 20 --dry-run
python tools/run_openxr_native.py --loader 'C:\path\to\openxr_loader.dll' --runtime 'C:\path\to\runtime.json' --seconds 20
```

The loader and optional runtime manifest paths must be absolute existing files.
The runtime override changes only the child process's `XR_RUNTIME_JSON`;
neither the registry nor the parent environment changes. A loader supplied with
SteamVR can load PiOpenXR without choosing SteamVR as the runtime.

`tools/run_openxr_native.py` runs the child with no console window, bounds its
lifetime, and writes `output.log` and `receipt.json` into a new timestamped
directory under `build`. `--output` selects a different new directory. Existing
captures are never overwritten. The receipt records executable/loader/manifest
hashes, duration, result and whether the watchdog fired. `--dry-run` creates no
files, directories, device or runtime instance. Local receipts can contain
machine paths; they are not public documentation.

For the Pimax native test, close Frontier and SteamVR, leave Pimax Play
running, and put on the headset. Run the harness against the installed PiOpenXR
manifest, face forward and move/rotate the head slowly. Expect the colored
triangle to remain fixed in space and appear in both eyes. The requested
interval is 1 to 60 seconds after the session starts. A 15-second READY
deadline, a 5-second shutdown deadline and an external watchdog cover different
stages; a blocked runtime call is covered by the watchdog rather than a claim
of graceful cancellation.

The harness requests exit from a running session, submits zero layers during
shutdown, waits for STOPPING and then ends the session. A passing process
requires at least one successful stereo layer, valid HMD location, normal
session stop and successful cleanup. A doff/runtime stop can end the diagnostic
early. These policies do not define what Elite should do on the corresponding
events.

## Desktop evidence and remaining gate

The full absolute-path build passed, including the new desktop gates, existing
ABI/paired-proxy WARP/LiveCopy and temporal/timing tests, Python tool
self-tests, and the 252-key config contract. No native runtime was opened by
these gates.

Focused tests pass: 996 stereo checks through the production renderer with fake
XR and real WARP resources; 29 harness argument/enumeration/geometry checks; 11
device-selection/validation checks; and 20 subprocess-runner checks. Counts
include repeated ABI/ownership checks, not that many independent scenarios.

The stereo checks read back pixels against independent ray/triangle
barycentrics for identity, translated, yawed and asymmetric views. They cover
both sRGB RTVs and explicit UNORM encoding, rotating acquired image indices,
unchanged pose/FOV metadata, defined background, partial initialization, count
growth, foreign devices, bad descriptors, both-eye acquire/wait/release
failures, positive pending loss, timeout, and cleanup errors. The runner tests
cover pass, nonzero exit, launch failure, timeout, child-only environment
changes and a dry run that leaves no output directory.

The native owner/loader path has now passed an ordinary session on PiOpenXR. It
has not been exercised through a complete fake loader. Its capability/session
creation and cleanup failure paths have source review, while the component
fixtures cover rendering and lifecycle failures. Do not interpret those
component tests or the successful native run as proof of complete native
initialization failure coverage.

The first PiOpenXR result and remaining visual/runtime checks are recorded in
the [native run notes](openxr-native-pimax-2026-09-12.md). A passing diagnostic
does not make Frontier launch without SteamVR: the game-facing OpenVR
interfaces, Elite-device binding, startup geometry cache/thread ownership,
stereo image capture, feature integration and broader lifecycle/runtime
qualification remain work for the backend.
