# Owned System interface and absolute-pose time

This checkpoint connects an owned historical `IVRSystem_012` object to the
native diagnostic. It follows the [validated device-binding and geometry
bootstrap](openxr-binding-geometry-2026-09-12.md). It is an internal
compatibility component, not a replacement `openvr_api.dll`: no
interface-discovery export advertises it, and the other three interfaces are
not implemented here. Frontier stays on the tested `f3c205e` proxy build.

Luna supplied the locator draft and interface tests. Parent review replaced
unsafe time arithmetic, corrected success-code and invalid-pose handling,
implemented the concrete interface/source contract, strengthened the assertions
and integrated the diagnostic. Initial tests that checked only
finiteness/non-success, discarded ABI results, or accepted a broad
diagnostic-count range were insufficient.

## Interface and state

`src/openxr/openvr_system.*` directly inherits the pinned Valve v0.9.20
`vr::IVRSystem` and defines all 44 exact virtual signatures, including the
fourth projection-convention argument and by-value matrices. It does not
inherit a forwarding wrapper or patch a foreign vtable.

`system_source.h` separates that ABI from the runtime owner. Each reader copies
one `SystemRead`: generation, connectivity, native geometry, adapter index and
metadata. Absolute-pose, reset and event callbacks receive the generation so an
owner can reject retired requests. The source must outlive the interface and
protect runtime/space lifetime. No callback may advance a frame or substitute
cached frame time for the requested pose time.

`system_publication.h` copies metadata and geometry together under a short
mutex. It rejects overlapping initialization, stale samples and retired
generations. Invalid tracking clears geometry while retaining the live HMD's
connectivity. No XR call or GPU work runs under that mutex. Copied records
remain values; publication does not itself protect XR handles from destruction.

| Methods | Current behavior |
| --- | --- |
| Size, raw FOV, projection, eye-to-head | Native asymmetric/canted geometry and caller clip planes/convention. Invalid or unavailable requests produce initialized zero outputs. |
| Absolute pose | Forward exact origin and prediction to the source; initialize caller-capacity entries. Only the HMD may be connected; unavailable tracking is invalid. |
| Reset seated origin | Delegate to the owner; diagnose unavailable behavior. The diagnostic owner does not implement reset yet. |
| Seated/raw-to-standing matrices | Return explicitly supplied transforms; otherwise zero matrices and a diagnostic. |
| Sorted devices, class, connectivity | Describe one live HMD. |
| Activity, distortion, D3D9, vsync | Defined unavailable results; physical-vsync seconds/counter are zero with false return. |
| DXGI adapter | Supplied index resolved from the validated device's adapter, or -1. |
| Desktop display / visibility change | Direct mode; reject mode mutations. |
| Hidden-area mesh | Empty mesh, allowing full-image rendering without an occlusion optimization. |
| Controllers/roles/haptics | No controllers; initialized failure outputs and diagnosed haptic requests. |
| Event polling | Validate byte capacity before dequeuing; return the associated event-time pose. The diagnostic source has no OpenVR event queue. |
| Input capture | Capture/release unavailable. Another-process capture requires explicit source knowledge; OpenXR focus alone does not establish it. |
| Debug, firmware, quit acknowledgments | Initialized failure outputs and diagnosed unavailable behavior; no game quit initiated. |
| `ApplyTransform` | Explicit invalid output and a diagnostic pending independent composition/velocity verification. This is a compatibility blocker. |
| Enum-name getters | Pinned historical names; numeric aliases use the first declaration. Unknown values have explicit unknown names. |

Deliberately unavailable operations report first use once per object/slot
through an atomic mask. Defined failures and empty optional data do not imply
feature parity. Required missing behaviors remain blockers before game-facing
discovery can enable this object.

## Properties

The pinned catalog supplies 81 typed property identifiers. Wrong getters return
`TrackedProp_WrongDataType`, unrecognized identifiers return
`TrackedProp_UnknownProperty`, and known unavailable values return
`TrackedProp_ValueNotProvidedByDevice`. Invalid/disconnected indices return
`TrackedProp_InvalidDevice` first. Matrix property failures follow the
historical header's identity-matrix rule.

The runtime name supplies `Prop_TrackingSystemName_String`; the OpenXR system
name supplies `Prop_ModelNumber_String` as an explicit compatibility mapping.
This is a runtime system label, not a hardware model-number field; see
[XrSystemProperties](https://registry.khronos.org/OpenXR/specs/1.0/man/html/XrSystemProperties.html).
HMD class, direct mode and unavailable physical-vsync reporting have coherent
answers. Display frequency requires an explicitly available, finite, positive
value; the diagnostic leaves it unavailable.

Manufacturer, EDID IDs, user-configured IPD and other unavailable hardware
values are not fabricated. OpenXR `vendorId` is not substituted for EDID, and
frame prediction period is not relabeled as refresh frequency. Strings report
required bytes including NUL, clear writable short buffers, respect caller
capacity and reject unterminated metadata. Error pointers may be null.

## Pose time and native integration

`head_locator.h` samples an injected Windows performance counter, converts it
through `xrConvertWin32PerformanceCounterToTimeKHR`, and adds the caller's
prediction rounded to nanoseconds. Floating-to-integer bounds and signed
addition are checked before overflow. It locates VIEW relative to the owner's
selected origin at exactly that time, independently of the frame's predicted
display time.

Only exact success continues dispatch. Pending and negative results are
preserved without publishing output. Validity requires both position and
orientation, finite coordinates and a unit quaternion. Partial tracking
produces initialized invalid geometry without reading unspecified fields.
TRACKED flags do not replace VALID flags. See the
[clock-conversion](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrConvertWin32PerformanceCounterToTimeKHR.html)
and
[space-location](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrLocateSpace.html)
contracts.

The native diagnostic enables clock conversion, reads runtime/system names and
resolves the adapter index. After a zero-layer frame it reads size, projection,
eye transform and model label through the concrete historical interface, then
requests an absolute HMD pose with zero prediction. A new `system_abi` line
reports interface version, valid absolute pose, clock source and adapter.
Passing requires this path before rendering. The bounded startup loop can retry
ordinary invalid tracking.

This diagnostic owner permits pose queries only on its existing runtime thread
and maps seated coordinates to unmodified LOCAL. It does not implement
coordinated game-thread init/shutdown, offset seated space, reset, STAGE/raw
mapping, event translation or velocity population. Velocity fields are
initialized to zero here, not measured. Those omissions must be resolved before
this becomes Elite's complete System implementation. The shipping launch-centre
policy is untouched.

## Validation and remaining gate

Focused tests pass 130 System assertions and 129 clock/pose assertions. Counts
include repeated contract checks. The System test receives its base pointer
through a separate translation unit so calls use the historical virtual ABI. It
checks both eyes/APIs, the three observed clip ranges, independent
coefficients/depth endpoints, typed errors, buffer canaries, event-time poses,
controller failures, retirement/re-init, 20,000 concurrent reads and exact
once-per-slot diagnostics. Locator tests cover signed predictions, rounding,
integer boundaries, dispatch failures, pending loss and invalid tracking. Both
tools have no-write dry runs and mandatory build gates.

The full absolute-path build passed, including these gates and the existing
stereo/binding/geometry, paired-proxy WARP/LiveCopy, export/ABI,
temporal/motion and GPU timing tests, Python self-tests and the 252-key config
contract. It opened no native runtime. The diagnostic executable and source
hashes are retained locally with the build log.

The updated diagnostic from `6be3d8d` subsequently passed its 20-second
PiOpenXR check. Its executable hash matches the retained full-build validation;
loader and manifest hashes match the previous inventory. Frontier and SteamVR
were absent before and after, and the runtime override applied only to the
child. Raw poses, paths and exact receipts remain local under `build`.

| Measurement | Observed result |
| --- | --- |
| Runtime | Pimax OpenXR, version 0.1.0 |
| Eye dimensions / format | 5424 x 5356 each; DXGI 29, RGBA8 sRGB |
| Device | D3D 11.1 on the requested adapter; interface adapter index 0 |
| Geometry bootstrap | Generation 1, sequence 1, zero prior stereo submissions |
| Owned System startup | `IVRSystem_012`; valid absolute HMD pose with prediction 0 via QPC-to-XrTime conversion |
| Model label | Runtime-backed string query succeeded; required length 20 bytes including NUL |
| Begun / stereo / zero-layer frames | 1800 / 1798 / 2 |
| Valid complete geometry / invalid samples | 1799 / 0 |
| Valid separately located HMD samples | 1799 |
| Normal stop / cleanup | Both succeeded |
| Process duration / result | Approximately 20.34 seconds; exit 0; no watchdog |

This exercises the new owned-interface bootstrap and absolute-pose clock
conversion on PiOpenXR. The absolute "now" query occurs at startup; the
ordinary frame loop continues to use its predicted display time.
Nonzero/historical predictions have desktop coverage, not new headset evidence
from this run. The user confirmed that the triangle appeared in both eyes,
remained fixed in space during head movement and closed normally. This passes
the ordinary visual and shutdown check for this diagnostic; game rendering and
broader lifecycle qualification remain open.

Next work remains export lifecycle/discovery, coordinated runtime ownership,
seated/recenter/velocity policy, Compositor/Chaperone/ExtendedDisplay behavior,
game texture capture/submission and EDVR feature integration. Pending six-face
skybox and lifecycle evidence remain relevant. This diagnostic component is not
ready for installation into Frontier.
