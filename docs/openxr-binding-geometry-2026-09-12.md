# Caller-device binding and startup geometry

This checkpoint follows the [first native PiOpenXR triangle
run](openxr-native-pimax-2026-09-12.md). It adds reusable session binding to an
existing D3D11 device, a geometry snapshot with coherent readers, and a
zero-layer bootstrap in the native diagnostic. Luna drafted the binding and
geometry components; parent review corrected partial cleanup, transform math,
validation and tests, then integrated them into the diagnostic and the desktop
graphics-proxy fixture.

The game-facing OpenVR interfaces and complete Elite transport are still
pending. Frontier remains on the previously tested `f3c205e` OpenVR build. This
checkpoint requires no game installation or configuration change.

## Device and resource ownership

`src/openxr/session_binding.*` borrows a live instance and typed dispatch
table, retains the caller's D3D11 device, and owns one session plus identity
LOCAL and VIEW spaces. It queries graphics requirements for the supplied
instance/system before creating the session, checks adapter LUID, minimum
feature level and device removal, and binds that exact device. It creates no
substitute device and loads no runtime itself. Texture validation compares
canonical COM device identity; another device on the same adapter is rejected.

Initialization requires the complete cleanup dispatch before acquiring
resources. Enumeration retries are bounded. Failure releases created spaces in
reverse order, then the session and retained device; the original
initialization result is preserved. Explicit shutdown attempts all cleanup and
reports the first error. Only a command's documented success codes can transfer
output-handle ownership. In particular, reference-space creation may create a
handle while reporting pending session loss, whereas session creation specifies
only `XR_SUCCESS`. See Khronos [session
creation](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrCreateSession.html),
[reference-space
creation](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrCreateReferenceSpace.html),
and [D3D11
requirements](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrGetD3D11GraphicsRequirementsKHR.html).

The caller must serialize access, keep the instance and dispatch alive, and
finish all frame/renderer use before destroying the binding outside `DllMain`.
This component does not implement backend-wide shutdown coordination or
recovery from device replacement.

`src/openxr/published_session.*` reads `gameDevice()` once from the existing
paired EDVR mapping and passes that candidate to the binding. A missing
publisher fails promptly before any XR call. The graphics proxy already retains
the first published device for process lifetime. This helper does not establish
the future paired-feature handshake or prove that the first device matches
every game texture; the backend must enforce those checks when integrated.

## Geometry and startup

`geometry_locator.h` runs only on the serialized owner during an already begun
frame. It locates two eye views in LOCAL and locates VIEW relative to LOCAL
using exactly the same predicted display time. It performs no wait, begin or
end itself and also works on no-render frames. Missing tracking-validity bits
remain an ordinary invalid sample; errors and positive pending statuses are
preserved.

`geometry_snapshot.h` retains the native eye poses, FOVs, HMD pose,
validity/tracked flags, dimensions, predicted time and generation/sequence. It
derives each eye-to-head matrix as `inverse(headToLocal) * eyeToLocal`,
retaining cant and translation. It never averages the eyes or substitutes
symmetric geometry. Nonfinite/nonunit poses, unsupported FOVs, invalid
dimensions, borrowed extension chains and unrepresentable output are rejected
without changing the caller's output. VALID bits are required; TRACKED bits are
retained without treating them as an additional validity requirement. This
follows the distinct validity/tracking meanings in [space
location](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrLocateSpace.html).

`system_geometry.h` publishes a complete snapshot under a short mutex with no
runtime call, graphics operation or wait under that lock. Fresh invalid
tracking clears availability. Old sequence/generation records cannot overwrite
newer geometry, and retirement prevents further publication. `SystemGeometry`
copies one immutable read transaction and exposes size, raw FOV, projection,
head and eye transforms using the existing historical OpenVR declarations. An
already copied transaction remains a value after the store changes; future
interface entry points must scope it within their own lifetime/owner guard.
This helper does not advertise or implement the full 44-method `IVRSystem_012`
interface.

The native harness now uses `SessionBinding` and these geometry components. It
completes a zero-layer frame before publishing the first valid snapshot and
before rendering the diagnostic triangle. It logs `bootstrap,ready=1` with
generation, sequence and `prior_stereo=0`; the pass condition requires this
bootstrap in addition to successful stereo, normal stop and cleanup. Its
`valid_views` counter now requires a complete valid eye/head snapshot, while
`valid_head` separately counts valid HMD locations. The original native run
predates this change and does not validate the revised bootstrap.

## Verification and next gate

The full absolute-path `build.bat` passed, including the existing ABI and
paired-proxy WARP/LiveCopy tests, temporal/motion and GPU timing regressions,
Python self-tests and the 252-key config contract. The new binding tests passed
450 checks, geometry passed 144, and actual graphics-DLL publication passed 22.
The native harness passed 24 desktop checks; the stereo renderer retained its
996-check gate. Counts include repeated ABI/resource assertions, not that many
independent scenarios. No native runtime was opened by this build.

The desktop tests exercise the production binding with fake XR calls and real
WARP devices: exact graphics binding, requirements order, partial
initialization, success-code ownership, bounded enumeration, cleanup failures,
reinitialization, foreign-device texture rejection, and same-time head/eye
location. A separate child loads the actual built `d3d11.dll`, creates WARP
through its exported entry point, verifies device publication and passes that
exact device through the binding with fake XR.

Geometry tests use independently specified matrices for translated/rotated
heads and canted eyes, global-transform invariance, invalid-output canaries,
stale/retired generations and concurrent snapshot readers. Both new tools have
no-write dry runs and mandatory `build.bat` gates. The updated native harness
has its own argument/enumeration tests; its full loader/bootstrap loop is not
replaced by a fake-loader integration test.

The revised diagnostic from `8ff0904` subsequently passed a 20-second PiOpenXR
run with Frontier and SteamVR absent before and after. Its executable hash
matches the locally retained full-build validation; the loader and manifest
hashes match the earlier inventory. Runtime selection applied only to the child
process. Raw tracking poses and machine paths remain in local receipts under
`build`.

| Measurement | Observed result |
| --- | --- |
| Runtime | Pimax OpenXR, version 0.1.0 |
| Eye dimensions / format | 5424 x 5356 each; DXGI 29, RGBA8 sRGB |
| D3D feature level | 11.1 on the requested adapter |
| Bootstrap | Generation 1, frame sequence 1, prior stereo submissions 0 |
| Begun / stereo / zero-layer frames | 1800 / 1798 / 2 |
| Valid complete geometry / invalid samples | 1799 / 0 |
| Valid separately located HMD samples | 1799 |
| Normal stop / cleanup | Both succeeded |
| Process duration / result | Approximately 20.38 seconds; exit 0; no watchdog |

This verifies the revised binding, same-time geometry acquisition and
publication before diagnostic rendering on the installed PiOpenXR
configuration. The user confirmed that the triangle appeared in both eyes,
stayed fixed in space during head movement and closed normally. This passes the
ordinary startup/stereo/tracking/shutdown check for the revised diagnostic; it
does not qualify game rendering or the remaining lifecycle cases.

The next game integration work is the owned historical OpenVR interface layer
and coordinated startup/frame ownership, followed by actual game texture
capture, submission and EDVR feature integration. Paired capability checks,
recenter/loss behavior and broader runtime qualification remain separate work.
Passing this diagnostic does not yet allow Frontier to launch without SteamVR.
