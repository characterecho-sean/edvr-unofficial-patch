# Elite on OpenXR: a drop-in `openvr_api.dll` that speaks OpenXR

*Design, reviewed against the source and API contracts on 2026-09-11. The
replacement transport and whole-frame GPU instrument are not implemented. The
session inventory below is retained from the original investigation; this
review checked source and vendor documentation, not new headset flights or a
fresh scan of the installed game. Phase 0 must preserve reproducible evidence
before an observation becomes a compatibility requirement.*

Implementation and subsequent evidence are tracked in
[openxr-implementation-status.md](openxr-implementation-status.md). The
[2026-09-11 Frontier census flight](openxr-flight-2026-09-11.md) adds measured
device and caller-thread evidence and records the startup capture limitation;
the original inventory below remains historical.

**Approval boundary:** this revision is for review. After Sean confirms it,
Luna agents can implement the bounded work packages below, with the parent
agent reviewing their changes and validation. Flights remain explicit gates; an
implementation or passing desk test is not a flight result.

## The ask

Today EDVR's `openvr_api.dll` is a proxy: it forwards exports to the real
runtime DLL underneath (Valve's, or OpenComposite's), wrapping
`VR_GetGenericInterface`, and patches two vtables in place to reach the frame
boundary and the submit (`src/openvr/openvr_proxy.cpp`, `compositor_hook.cpp`,
`system_hook.cpp`). The proposal is to stop forwarding. EDVR's file becomes the
OpenVR runtime as far as Elite is concerned, implements the interfaces the game
asks for, and speaks OpenXR to whatever runtime the machine has. Elite keeps
calling OpenVR and nothing in the game changes.

Two requirements come with it:

1. **OpenXR exclusively.** No SteamVR-native forwarding path is kept once the
   layer is trusted. SteamVR is itself an OpenXR runtime, so SteamVR users are
   served through it.
2. **The Monitor keeps useful, measured GPU figures.** App GPU time must work
   independently of the runtime. Core OpenXR does not provide the equivalent of
   OpenVR's compositor timing record. Exact compositor GPU time, drops and
   reprojection are optional capabilities, not numbers to manufacture from
   cadence. Preserve the existing SteamVR measurements during migration.

## Where this stands against the 2026-09-01 goal

The goal stated on 2026-09-01, during the render-scale flights, was to support
native SteamVR only and drop OpenComposite, because much of EDVR's
transport-safety machinery exists for OpenComposite-over-VDXR failures: the
cull guard's two-stage go-live and canonical-size snap, the copy-not-bounds
crop (`guard_crop.h`), the early handover (`early_session.cpp`), the launch
centre (`launch_centre.cpp`).

This retains the aim of dropping OpenComposite, but changes the proposed
transport from native SteamVR to OpenXR. That is a deliberate scope change. The
README records a Quest 3 over Virtual Desktop and a Pimax Crystal Super over
PiOpenXR, both through OpenComposite, as field-verified configurations; it does
not establish the size of any runtime's user population.

Owning the transport removes the need to patch foreign OpenVR objects. It does
not remove Elite's terrain-culling defect, render-size transitions, tracking
discontinuities, or the graphics half's hooks. Retire each workaround only
after its replacement preserves the behaviour it protected.

## What Elite needs from `openvr_api.dll` (recorded inventory)

The original document reports these measurements on 2026-09-11: the delay-load
import table of `EliteDangerous64.exe` in the Steam install, parsed with a
stdlib PE reader in the manner of `tools/gen_exports.py`; the `IVR*_nnn`
literals from a string scan of the same binary (also recorded at the top of
`compositor_hook.cpp`); the export tables of the game's `openvr_api_orig.dll`
and of EDVR's proxy; and the `VR_GetGenericInterface("...")` lines the proxy
writes, in the three newest VR logs across the two installs (Frontier
2026-09-10 19:51 and 20:06, Steam 2026-09-11 07:42).

| What | Recorded observation or source check |
|---|---|
| How the DLL is loaded | delay-load import: mapped at the first call, about 1.2 s after the game's device exists, and never at all on the Oculus path |
| Exports imported | 5: `VR_InitInternal`, `VR_ShutdownInternal`, `VR_GetGenericInterface`, `VR_IsInterfaceVersionValid`, `VR_GetInitToken` |
| Exports in the file | 14 in Valve's: the five above, `VR_IsHmdPresent`, `VR_IsRuntimeInstalled`, `VR_RuntimePath`, three error-string calls, `VRControlPanel`, `VRDashboardManager`, `VRTrackedCamera`. 16 in EDVR's, with two self-test exports |
| Interface literals in the binary | `IVRSystem_012`, `IVRCompositor_014`, `IVRChaperone_003`, `IVRExtendedDisplay_001`, `IVROverlay_011` |
| Interfaces requested in a session, in order | `IVRSystem_012` at t=0; `IVRExtendedDisplay_001` within 15 ms; `IVRCompositor_014` 2 to 2.5 s later; `IVRChaperone_003` up to 3 s after that |
| Never requested in any log | `IVROverlay_011`. The one `IVROverlay_028` request on record came from something injected, not the game (`openvr_proxy.cpp`) |
| Compositor re-requests per session | 2 to 3, each a few seconds before the eye textures change size ("ONE EYE ... CHANGED") |
| Methods across the four requested interfaces | 84 in Valve's 0.9.20 header: System 44, Compositor 29, Chaperone 8, ExtendedDisplay 3. Hook coverage is not a census of what the game calls |
| System calls per frame | `GetRecommendedRenderTargetSize`, `GetProjectionMatrix`, `GetProjectionRaw`, `GetEyeToHeadTransform`, each about 12 times a frame (~1080/s at 90 Hz: `system_hook.cpp`) |
| Compositor calls per frame | `WaitGetPoses` once, `Submit` twice; `SetSkyboxOverride` once at startup with a 1x1 texture (OpenComposite's log, `early_session.cpp`) |
| Projection planes asked for | 0.025..50000 for the scene and 0.1..1000 for something else (`system_hook.cpp`) |
| Tracking space | seated (the launch centre's reset reached it) |
| Eye-to-head rotation | dropped by the game (docs/canted-projection.md) |
| Submit shape | one texture per eye, null bounds, on both rigs now; one double-wide texture with per-eye bounds on a Quest 3 over Steam Link, 2026-08-17 (`noteEyeTextureSize`) |
| Submit format support in EDVR | the 8-bit RGBA and BGRA family: typeless, UNORM and sRGB (`supersample_resolve.cpp`). Supported formats in code do not prove which formats each rig submits |
| Threads | Submit and Present on one thread (measured 2026-08-15); VR init off the render thread (measured 2026-08-29); WaitGetPoses' thread not yet logged |
| The environment on the two installs, latest logs | Steam: Valve's DLL, 4536x4480 recommended per eye. Frontier: OpenComposite, 5424x5356 recommended per eye, and "the recentre DID NOT TAKE" |

The replacement preserves the recorded 14-name export surface and EDVR's
self-test exports, with explicit signatures and failure behaviour rather than
generic zero-return thunks. `VR_IsInterfaceVersionValid` and
`VR_GetGenericInterface` must share one support table: initially the **four
implemented interfaces**, not all five binary literals. `IVROverlay_011` and
unknown versions return unsupported without a fatal dialog. Re-requests return
the same object within an initialization generation; init tokens invalidate
cached state across shutdown/re-init.

Use the exact [Valve 0.9.20
header](https://raw.githubusercontent.com/ValveSoftware/openvr/v0.9.20/headers/openvr.h),
whose declarations were counted for this review. `IVRSystem_012` has the fourth
projection-convention argument and by-value matrix returns. Pin the header and
test the real C++ member ABI, struct packing, buffer lengths and enum values.
`openvr_min.h` and the eight-slot fake system are not complete implementations
of that interface. Modern submit flags used internally by EDVR must not be
mistaken for features promised by the old game's ABI.

## What the VR half does at those seams today

These behaviours need explicit owners under the new transport. This is a
preservation inventory, not a claim that the current hook bodies can be moved
unchanged.

At the frame boundary (`hookedWaitGetPoses`): the launch centre; the gaze probe
and the gaze source; the compositor timing read; the pacing statistics;
clearing the glitch mark and the submit-pair latch; the transition hold; the
pose ring and the pose hold; the ship-forward publication; the head offset
(Explorer Cam); the theater's freeze; the cull guard's stage transitions and
the temporal jitter (`systemHookFrameBoundary`); the wait time for the Monitor.

At the door (`hookedSubmit`): the ordinary path applies temporal, guard crop,
supersample resolve, sharpen, then menu. Theater/heal and arrival-mono paths
branch earlier; withholding either reprocesses a raw shadow through crop,
resolve, sharpen and menu, or forwards no image. Snapshot mode captures before
the ordinary passes; the usual resubmit shadow captures the game's full raw
texture after a successful forward. Preserve eye-size/bounds publication and
temporal invalidation on every branch. The present GPU bracket covers selected
passes, not all work in these branches.

At the system interface: observing the size, both projection forms and
eye-to-head; the guard's lie and the temporal jitter, told through the raw
thunk and the matrix receiver; the receiver's formula check; the plane pairs;
`predictDisplayPose` reading three further slots. Around all of it: the early
session handover, the runtime detection by exports, the interface suppression
shield, the compositor timing layout sniffing.

And the scaffolding that exists only because these are hooks on objects EDVR
does not own: slot tables marked unverified, argument-shape validation before a
slot is trusted, crash sentinels around every install, the owner-identity test
on every call, the reclaim pass and its vouching, the executable-prefix range
checks and the convention-capture thunks in `system_thunks.asm`. These can go
from the replacement OpenVR backend. Keep argument validation, fault reporting,
resource ownership checks and recovery behaviour where they still apply. The
graphics half and proxy build continue to need `vtable_hook.*`; do not delete
shared hook machinery as part of retiring the VR hooks. Old comments about the
6c rule are historical: current code deliberately calls selected system slots.

## Pros

- **Owned OpenVR interfaces.** Exact C++ implementations replace foreign vtable
  interception and its ABI recovery paths.
- **Direct OpenXR transport.** Native OpenXR runtimes can receive frames
  without OpenComposite. SteamVR remains reachable through its OpenXR runtime;
  equal performance, startup and image quality require measurement.
- **Coherent projection state.** A frame record can tie together tracking,
  game-facing projection, crop, temporal processing and submitted geometry.
  These are related representations, not always identical FOVs.
- **Explicit image metadata.** OpenXR submission can carry the pose and FOV
  appropriate to the final image. EDVR must retain and transform that metadata
  through every substitution; the API does not infer it from pixels.
- **Optional runtime features.** A session EDVR owns enables gaze actions and
  other extensions where the selected runtime supports them. Quad menu layers,
  depth and direct rendering into swapchain images are later optimizations.

## Cons and limits

- **EDVR becomes responsible for compatibility.** Session lifecycle, adapter
  selection, frame ordering, events, poses, properties, loading screens and
  texture conversion currently have another implementation underneath them. An
  unobserved method is not necessarily safe to ignore.
- **The runtime matrix expands.** Qualify SteamVR OpenXR, VDXR and the actual
  Pimax runtime installed on the desk. PiOpenXR and the separate [PimaxXR
  project](https://github.com/mbucchia/Pimax-OpenXR) are distinct
  implementations; record manifest path, runtime name/version, headset and
  transport rather than using their names interchangeably. Varjo and reachable
  Meta OpenXR configurations require field coverage.
- **Some Monitor data will remain unavailable.** Local GPU queries measure the
  application's command stream, not the compositor or the headset's actual
  presentation outcome. Optional timing experiments must not block the core
  transport or appear as measured results before validation.
- **Feature preservation is substantial work.** Terrain overscan, temporal AA,
  Explorer Cam, theater/heal, loading panels, menu anchoring and transition
  withholding all depend on the current pose/projection/submit contract.
  Removing OpenComposite alone does not establish correctness for any of them.
- **Oculus-native Elite remains outside scope.** It does not load this DLL. The
  replacement also does not promise general OpenVR compatibility, controller
  support, overlay support, other graphics APIs or quad-view stereo.
- **Migration and distribution need work.** The current build generates exports
  from the game's DLL; the installer backs up and chains an existing runtime. A
  standalone backend needs its own reproducible export definitions, loader
  dependency, notices and install/rollback tests.

## The layer, specified enough to implement

### ABI and initialization

Implement the four versioned interfaces in an isolated compatibility module,
using the pinned historical SDK. Keep backend-independent EDVR frame logic
behind internal C++ contracts. Calls from EDVR itself should use those
contracts rather than asking its own OpenVR facade for newer interfaces or
patching its own objects. In particular, replace `gazeProbeNoteGetter`,
`systemInterfaceV012` and `predictDisplayPose` consumers deliberately.

Initialization and shutdown run outside `DllMain`, as the current proxy's lazy
loader already requires. Define repeated init, failed init, shutdown and
re-init behaviour, error strings and init-token changes in desk tests. Reject
unsupported application types and interface versions with the historical API's
error values. Presence/runtime probes must not create a temporary session just
to answer a boolean.

The first backend supports D3D11, primary stereo and an opaque blend mode.
Require `XR_KHR_D3D11_enable`; enumerate and validate the view configuration,
blend modes and graphics requirements before creating rendering resources.

### Device and session ownership

Require a matching EDVR `d3d11.dll` for the first implementation. With no
published device, fail initialization promptly with a useful error; do not wait
indefinitely or create a throwaway device/session.

`gameDevice()` is currently a raw pointer in the versioned shared mapping (the
field was introduced in v20; the mapping is now v30). In
`src/d3d11/d3d11_proxy.cpp::attachToDevice`, the first device wins and gets a
process-lifetime `AddRef`. This guarantees lifetime, not that it is Elite's
submitted-texture device. The two-device incident in [the startup
review](review-opencomposite-startup-2026-09-10.md) is a reason to verify
identity, not proof that device selection is already solved.

Before `xrCreateSession`, compare the candidate's adapter LUID and feature
level with `xrGetD3D11GraphicsRequirementsKHR`. Retain an owned reference and
validate each submitted texture against that device. The runtime requires a
compatible device; matching the adapter alone does not make resources on two
D3D11 devices interchangeable. [D3D11 binding
contract](https://raw.githubusercontent.com/KhronosGroup/OpenXR-Docs/main/specification/sources/chapters/extensions/khr/khr_d3d11_enable.adoc).

Phase 0 logs all created devices and the devices of the first skybox and eye
textures. If the first-device policy fails, revise publication/selection before
making the backend usable on that configuration. Do not silently copy across
devices or rebuild during `Submit`. A removed or replaced device stops
submission and reports a restart requirement in the first implementation. Keep
one session during ordinary startup and resolution changes; this is not a
promise that a lost session can live forever.

### Session state and startup geometry

Pump events during initialization and idle periods as well as frame calls;
progress must not depend solely on the game calling `PollNextEvent`.

| OpenXR state | Backend behaviour |
|---|---|
| IDLE | Poll events; no frame calls; return initialized, invalid poses if no valid tracking exists |
| READY | Begin the session once and start frame synchronization |
| SYNCHRONIZED / VISIBLE / FOCUSED | Keep wait/begin/end running; render only when `shouldRender` and view validity permit |
| STOPPING | Finish/abandon outstanding work according to handle validity, call `xrEndSession`, then wait for further state events |
| LOSS_PENDING / instance loss | Retire affected handles and histories; first version reports loss and requires restart |
| EXITING | End the XR experience; do not automatically restart it |

STOPPING can occur on headset disengagement and is not itself a request to quit
Elite. FOCUSED controls XR input; lack of focus does not imply an invisible
application. These distinctions come from the [session-state
contract](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrSessionState.html).
Translate terminal loss/exit into OpenVR events only after the census confirms
Elite's reaction. Never terminate the game from the bridge.

Size can be queried from view-configuration recommendations before drawing;
per-eye FOV and eye-to-head geometry come from located views, not that size.
Elite requests system/extended-display interfaces before its compositor. Phase
0 must capture its first geometry calls, and the desk harness must prove how
the backend obtains valid geometry before answering them. A bounded startup
pump with zero-layer frames is a candidate. Do not invent symmetric FOVs, treat
invalid views as valid, or block an init thread indefinitely waiting for the
render thread it is preventing from starting.

### Frame ownership and submit pairing

Use one frame record with an increasing sequence, predicted display time,
`shouldRender`, tracking validity, reference-space generation, real tracking
pose, game-facing pose, per-eye render/output geometry and eye completion mask.
System queries read a coherent snapshot; repeated getters cannot change the
frame. Define the bootstrap snapshot separately from a begun render frame.

`WaitGetPoses` closes any prior incomplete frame without a projection layer,
then waits/begins and locates the next frame. Locate the HMD using a VIEW space
relative to the selected tracking space; do not derive head position by
averaging the eye views. Fill both OpenVR pose arrays within caller capacity;
non-HMD devices are disconnected/invalid. `GetLastPoses` reads the cache and
must not advance the frame. Absolute-pose queries use their requested time and
origin, or report invalid data when unavailable. Invalid tracking must not be
returned as `Running_OK` merely because an old matrix exists.

Each `Submit` validates eye, flags, texture, bounds and device, then captures
that eye's content on the owning immediate context before returning. Keeping
only the first texture pointer until the second eye arrives is insufficient:
Elite reuses textures, and `AddRef` protects lifetime, not pixel contents.
Accept left-first or right-first; reject a duplicate eye without replacing the
accepted eye. The second distinct eye completes the pair. End exactly one frame
with both projection views, or zero layers if the pair is incomplete, invalid
or intentionally withheld without a usable stereo shadow. A stereo projection
layer cannot contain only one view. [Projection-layer
contract](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerProjection.html).

`shouldRender == false` still requires frame synchronization while running;
omit layers and avoid EDVR's expensive post-processing. The bridge cannot
assume that Elite itself will stop drawing. [Frame-state
contract](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrFrameState.html).
`PostPresentHandoff` may be a no-op only after the census/harness establishes
that pair completion and the next wait close every frame. A submit without a
begun frame is rejected; startup skyboxes are a separate path.

Serialize frame transitions and teardown without holding a lock across a
blocking wait when another required call needs that lock. OpenXR allows
`xrWaitFrame` on a different thread, but concurrent waits require external
synchronization. Test actual Elite ordering, shutdown during a wait and session
events during an incomplete pair. [Frame
synchronization](https://raw.githubusercontent.com/KhronosGroup/OpenXR-Docs/main/specification/sources/chapters/rendering.adoc).

### Swapchains, bounds and colour

Start with one swapchain per eye and a final copy/blit from EDVR's existing
pass outputs. For each accepted eye: acquire, successfully wait, enqueue its
copy/blit, then release. Only use a swapchain image while it is acquired and
ready. Track acquisition and release state on every failure path, including
positive timeout/loss statuses; a timeout is not permission to write or release
an unwaited image. [Release
contract](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrReleaseSwapchainImage.html).
Follow the D3D11 binding's GPU submission requirements before release; avoid
CPU waits for GPU completion as normal frame-loop policy.

Validate null/full bounds, double-wide subrectangles, and reversed U and V.
Compose the game's bounds with the cull-guard crop once. Copies cannot flip,
resize or convert between RGBA and BGRA: use a shader blit when needed. Check
sample count, array/mip shape, dimensions and resource formats; implement an
explicit MSAA resolve or reject unsupported input. Preserve immediate-context
state and EDVR's binding shadow around new rendering work.

Honour `Texture_t::eColorSpace` before applying Auto: Gamma uses an sRGB
interpretation, Linear a linear interpretation; Auto follows the historical
8-bit rule. Select an enumerated concrete swapchain format, with an explicit
conversion path if the desired family is unavailable. Compatible UNORM/sRGB
resource copies preserve bits; shader SRV/RTV choices can decode or encode
them. Check ramps and known colours so a pass does not apply gamma twice.
OpenXR's [swapchain colour
rules](https://raw.githubusercontent.com/KhronosGroup/OpenXR-Docs/main/specification/sources/chapters/rendering.adoc)
determine how the runtime interprets those bits.

A recommended size is not a requirement to resize every submitted image. Key
swapchains by actual outgoing image requirements, keeping size, crop and FOV
consistent; validate against device/runtime limits. Recreate affected
swapchains at a safe frame boundary when format or dimensions change, without
recreating the session. Compositor interface re-requests alone do not trigger
resource destruction.

Writing the final EDVR pass directly into a runtime image is a later
optimization. Existing passes return owned textures and rely on particular
SRV/UAV/RTV formats. Direct output requires destination-aware pass APIs and
runtime-supported usage flags; a runtime image must never become a persistent
temporal history or published source texture. No copy saving is promised in the
first implementation.

### Projection and pose semantics

Retain separately: runtime view geometry; game render projection including
overscan/jitter; and final submitted image pose/FOV after crop, temporal
reconstruction or a substitution. One immutable frame record links them. The
compositor must receive geometry describing the outgoing pixels, not blindly
the widened or jittered projection the game queried.

For the normal parallel-eye case, convert OpenXR angles to tangents, preserving
this codebase's raw order: left/right are `tan(angleLeft/angleRight)`, raw
`pfTop` is `tan(angleDown)` and raw `pfBottom` is `tan(angleUp)`. Verify
against asymmetric vertical fixtures, the independent projection matrix and
[the menu projection regression](review-opencomposite-startup-2026-09-10.md).
Support the historical projection-convention argument explicitly. Apply
render-size scaling, cull coverage and jitter once at the same frame boundary,
including their publications to the graphics half.

OpenVR returns one recommended per-eye width/height pair. If OpenXR recommends
different sizes for the eyes, choose a size that accommodates both, then apply
the existing scale policy within device/runtime limits. Derive eye-to-head
transforms from head and eye locations at the same time and in the same space.

Canted displays require a distinct implementation gate. Returning eye rotation
unchanged does not fix a game that discards it. The proposal in
[canted-projection.md](canted-projection.md) deliberately gives the culler
bounded raw tangents and the rasterizer a rotated matrix. Those cannot both be
represented by one identical four-tangent FOV. Preserve a tested parallel
projection configuration for initial parity, or implement and validate the
matrix fold and all consumers before claiming PP-off support. This port does
not automatically deliver the proposed pixel savings.

Pose modes also need separate contracts. `forwardSubmit` currently implements
headset pose-hold by tagging the frozen image with a freshly predicted headset
pose to reduce reprojection; substituting the old render pose would change that
instrument. Theater output is rendered through `theaterXform` using real and
frozen poses. Explorer Cam applies a deliberate camera offset. Trace each
mode's final pixels back into the composition space, retaining its intended
world/head lock rather than assuming every freeze should use the same pose. Use
`XrCompositionLayerProjectionView::pose` and, where appropriate, layer space to
express that result; validate ordinary tracking, both hold modes, theater and
Explorer Cam independently.

### Spaces, recentre and events

Maintain an unmodified LOCAL base, a VIEW space and an offset LOCAL space for
seated coordinates. `ResetSeatedZeroPose` establishes the current position and
yaw as the seated origin while preserving gravity alignment. Replace offsets
relative to the base, not cumulatively relative to the already-offset space.
Support standing via STAGE when present; define a documented fallback and
consistent seated/standing transforms otherwise. Raw tracking has no exact
portable equivalent; any emulation must be explicit and tested if called.

Handle `XrEventDataReferenceSpaceChangePending` at its `changeTime`, with its
validity and previous-space transform. It is a reference-space change, not
always a user recentre. Transform retained anchors or invalidate affected pose
rings, temporal history and stereo shadows together at a frame boundary.
[Reference-space
contract](https://raw.githubusercontent.com/KhronosGroup/OpenXR-Docs/main/specification/sources/chapters/spaces.adoc).

Preserve `fix.launch_centre`'s user choice: today `auto` enables it only under
OpenComposite, `on` enables it explicitly and `off` disables it. A
runtime-independent implementation needs an agreed new Auto policy; do not
silently centre everyone on first pose, particularly users who chose off.

Synthesize only events whose OpenVR semantics are defined and exercised. A
FOCUSED transition alone does not prove a dashboard opened or closed; an
arbitrary eye-to-head change is not necessarily an IPD change. Preserve event
buffer sizes, queue ordering and the corresponding state/property answers.

### Properties, display compatibility and optional interfaces

Implement a minimum coherent HMD, including connectivity/class, requested
properties, DXGI adapter selection, tracking origin, pose caches and focus.
Unknown properties return the appropriate property error; strings respect
required-size and termination rules. Do not fabricate physical vsync timing or
successful compositor timing to fill unsupported methods.

`IVRExtendedDisplay_001` has three output methods: window bounds, eye viewport
and DXGI output information. It has no method that simply reports "no extended
mode". Define safe direct-mode/virtual-display answers and verify Elite accepts
them. `IVRChaperone_003` returns available stage bounds or failure with
initialized outputs; it must not claim calibrated bounds when none exist. The
baseline excludes `IVROverlay_011` consistently from both discovery calls.

Count all 84 methods, but specify every method's behaviour even if uncalled.
Prioritize any observed skybox, fade, clear-frame, suspend-rendering and
low-resource calls: returning success without doing their requested work can
break loading transitions. Log first use of a stub once; initialize output
buffers and return a defined failure when one exists. If a required behaviour
cannot be implemented, it is a release blocker rather than a silent no-op.

### Withholding and history

The current `resubmit_shadow.cpp` holds raw game textures per eye, with no
associated pose, FOV, bounds, colour interpretation or stereo generation. It
cannot simply become the source of an OpenXR projection layer with the current
frame's metadata.

Define a last-accepted **stereo** shadow with image data and matching geometry,
space generation and colour. Promote both eyes atomically only after a
successful complete submission; successful submission is not proof of display.
Re-show it with its appropriate stored metadata, or deliberately re-render it
under the selected hold/theater semantics. Copy it into acquired images when
needed; retaining a released runtime image as writable storage is invalid. With
no compatible pair, submit zero layers. Invalidate on device, size, projection
or coordinate changes unless an explicit transform preserves it. Keep the
existing temporal-withheld notification and verify consecutive withholds,
camera-return detection, snapshot mode and recovery independently.

### Runtime selection and extensions

Begin with `auto`: honour an inherited `XR_RUNTIME_JSON`, otherwise use the
64-bit active-runtime registry selection. Any later EDVR override is a
startup-only absolute manifest path resolved before loader discovery, not a
runtime DLL path or a live switch. Friendly runtime names require reliable
manifest discovery; do not hardcode a Pimax name to the wrong implementation.
Log selected manifest where known, runtime name/version and enabled extensions.
Do not edit the machine-wide registry. [Loader selection
rules](https://registry.khronos.org/OpenXR/specs/1.1/loader.html).

Pin the loader/headers and verify static linking with this MSVC build. If a
shared loader is needed, include it in packaging, installer ownership and
uninstall tests. Keep third-party notices for the exact versions used. Reuse
permissively licensed SDK declarations; do not incorporate OpenComposite
implementation code into this MIT project. A separate OpenVR backend build
initially provides an easier rollback boundary than a live transport switch.

Optional features are separate capabilities, not prerequisites for frames:

| Capability | Contract and scope |
|---|---|
| `XR_KHR_win32_convert_performance_counter_time` | Correlate QPC and XrTime; never cast between clock domains |
| `XR_EXT_eye_gaze_interaction` | Check system support; create an action set and pose action, suggest gaze bindings, attach the set, create an action space, sync and validate gaze, then convert to EDVR's head-relative ray |
| `XR_KHR_visibility_mask` | Convert the hidden-triangle mask to the old OpenVR representation and current projection; return an empty mesh until that conversion is correct |
| `XR_FB_display_refresh_rate` | Physical display Hz where offered; absence does not make predicted frame cadence a measured display rate |
| `XR_KHR_composition_layer_depth` | Later feature: submit depth matching final colour, projection, crop, temporal treatment and pose |
| `XR_META_performance_metrics` | Enumerate actual counter paths, enable collection and check units/validity; no desktop runtime support is assumed |

Gaze still needs OpenXR actions even though the game does not use controller
actions. Preserve the current validity/blink handling and fixed-centre
fallback. Availability of the extension alone does not prove tracking is active
or correct on that runtime. [Gaze
extension](https://raw.githubusercontent.com/KhronosGroup/OpenXR-Docs/main/specification/sources/chapters/extensions/ext/ext_eye_gaze_interaction.adoc).

Reversed/infinite depth is representable: `minDepth=0`, `maxDepth=1`,
`nearZ=+infinity`, `farZ=physicalNear` describes an infinite reversed mapping.
The unresolved issue is whether EDVR can supply matching final depth for all
relevant pixels, including UI, temporal reconstruction and substituted frames.
Omit depth unless it can. [Depth
contract](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerDepthInfoKHR.html).

## The Monitor page's GPU figures

`frame_timing.cpp` reads validated legacy/modern OpenVR records. The Monitor
already has independent per-eye door GPU queries and CPU/Present sampling; not
every GPU figure comes from the compositor. See
[`perf_monitor.h`](../src/d3d11/perf_monitor.h),
[`perf_monitor.cpp`](../src/d3d11/perf_monitor.cpp) and
[`frame_timing.cpp`](../src/openvr/frame_timing.cpp).

Build a runtime-independent **app GPU span** first, in the current proxy. It
measures elapsed GPU time across an identified application command-stream
interval, including bubbles and contention. It is not GPU busy time, total
system cost or the compositor's own timing definition.

| Readout | Proxy migration | OpenXR backend |
|---|---|---|
| App GPU | New measured span alongside the existing SteamVR scene measurement | Same instrument, ending after final eye transfer |
| EDVR GPU | Preserve per-eye pass intervals; audit uncovered branches/copies | Sum measured EDVR intervals for the same stereo frame, including final transfer |
| GPU TIME | Keep SteamVR's measured total and label local-span fallback | Label as app GPU span; do not imply compositor cost is included |
| Compositor GPU | Existing validated SteamVR record | Unavailable unless a separately validated runtime source exists |
| Dropped / reprojected | Existing validated SteamVR counters | Unavailable without an actual runtime source |
| App cadence | Present and pose-wait intervals, clearly labelled | Add predicted cadence as a separate diagnostic |
| Display Hz / budget | Existing property when available | Refresh-rate extension when available; otherwise unknown physical Hz and an explicitly labelled predicted app budget |
| CPU / wait / Present | Existing measured clocks | Preserve thread-time fallback; time `xrWaitFrame` separately |
| GPU utilization / temperature | Existing NvAPI | Same; optional PDH process-engine data is a distinct readout |

### Bracket placement and query ownership

- **Start:** publish a frame sequence at the pose boundary. The graphics half
  consumes it on the game's immediate-context execution path, inserting a
  timestamp before the first covered GPU command. Inventory draws, dispatches,
  clears, copies, resolves and `ExecuteCommandList`; a draw hook alone does not
  prove coverage of the first command. Exclude EDVR's own instrumentation from
  recursively opening a bracket. If complete coverage is impractical, label the
  narrower interval honestly.
- **End:** after the second distinct eye's final work/transfer on that context.
  In the proxy, identify whether the marker includes runtime submit work and
  document that boundary. This excludes any mirror/post-submit work after the
  marker; it is not automatically the whole Present-to-Present GPU workload.
  Missing eyes or a new boundary retire an incomplete sample as invalid.
- **EDVR cost:** keep per-eye intervals around EDVR work, including relevant
  early-return branches and copies. Do not call everything between first-eye
  door entry and second-eye completion "EDVR": the game may render between
  submits. App-minus-EDVR is not an exact game-only measurement without
  matching coverage and a valid accounting model.
- **Threading:** all query issue/poll calls use the owning context's serialized
  execution path. An atomic boundary flag makes publication safe, not the
  immediate context or query ring. Microsoft explicitly requires one thread at
  a time on an immediate context. [D3D11
  threading](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-intro).
- **Queries:** use a bounded ring keyed by device and frame sequence, with
  explicit open/pending/invalid states. Poll older samples with
  `D3D11_ASYNC_GETDATA_DONOTFLUSH`; pending stays pending, failure/disjoint
  retires the sample, ring exhaustion skips measurement. Never flush or wait
  for telemetry. Avoid overlapping disjoint scopes when combining the frame and
  door instruments; additional timestamps can share a valid outer scope.
  [Timestamp
  validity](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_query_data_timestamp_disjoint).
- **Publication:** attach both eye timings to their original frame, not the
  latest completed sample from each eye. The current door ring stores only
  latest per-eye milliseconds, and the Monitor rings frames at Present;
  widening that implementation unchanged would mix delayed GPU results with
  newer frames. Publish source, sequence, validity and age. Clear stale values
  on loss/re-init and version any shared-layout change in both DLLs.

Validate with a controlled D3D11 workload, query failures/disjoint data, ring
pressure and two-device ownership first. Then compare local and SteamVR timing
on matched frame sequences, accounting for the compositor's delayed records.
Explain systematic differences rather than demanding numerical equality.
Measure telemetry overhead with it enabled/disabled. This validates the
instrument; other runtime/device combinations still need their own checks.

### Measurements that remain optional

A predicted display-time jump or a two-period cadence is not proof of a dropped
frame, motion smoothing or reprojection. The runtime can change its predicted
period independently of physical refresh. If displayed, name these observations
"predicted cadence" or "prediction gaps", never dropped/reprojected counts. The
existing drop attribution must not treat them as compositor evidence.
[XrFrameState
semantics](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrFrameState.html).

A SteamVR timing sidecar is a research spike. Start in a separate helper
process with Valve's runtime; do not load a second OpenVR initialization into
Elite or assume a background client's record refers to Elite's OpenXR frame.
Prove application identity and frame correlation before adding an optional
source. No sidecar is required to ship the app-span instrument.

For Meta metrics, enumerate supported paths and verify collection, units,
validity and freshness per counter. A headset vendor name does not establish
that its Windows runtime exposes this extension. [Counter
enumeration](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrEnumeratePerformanceMetricsCounterPathsMETA.html),
[counter
queries](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrQueryPerformanceMetricsCounterMETA.html).

PDH process-engine utilization is an optional later sampler. It is not the same
quantity as NvAPI's adapter load and does not supply temperature. Specify PID,
adapter/engine selection and aggregation; do not sum unrelated engines into a
misleading percentage. Preserve unavailable values when there is no source.

## Migration decisions

1. **Separate build first.** Keep the shipping proxy default while the OpenXR
   backend is qualified. Select transport before initialization; no live switch
   or silent fallback after an OpenXR session fails. Keep a documented rollback
   artifact. Retire the proxy only after acceptance gates, not an arbitrary
   number of releases.
2. **Paired DLLs required.** Match shared protocol/build capabilities and
   verify the device before session creation. No openvr-only temporary-session
   path.
3. **Preserve configuration until retirement.** The table below records
   candidates, not authorization to remove settings in the first code change.
   Keep `edvr.ini`, config audit, generated menu/installer schema and support
   documentation synchronized when a migration is implemented.
4. **Baseline before optimizations.** No mandatory sidecar, PDH, direct
   swapchain pass output, PP-off fold, depth layer or quad menu in initial
   transport parity. Gaze migration is required before claiming parity for
   configurations that use EDVR's current eye-driven foveation.

| Existing key | Current behaviour | Proposed OpenXR treatment |
|---|---|---|
| `advanced.real_openvr_dll` | Chooses the forwarded DLL | Proxy-only during migration; retire with proxy |
| `advanced.suppress_interfaces` | Refuses configured interface prefixes before reaching the runtime | Keep proxy behaviour; owned backend has a fixed supported-interface table |
| `advanced.compositor_timing` | Enables existing compositor timing collection | Define separate measured-source behaviour before changing this switch; never make local queries depend accidentally on a legacy timing decoder |
| `fix.vr_handover` | `early` submits a 1x1 texture before the game's compositor calls; `stock` does not | Not used by owned session initialization; retire only with proxy |
| `fix.launch_centre` | `auto` is OpenComposite-only; `on`/`off` explicit | Preserve explicit choice; review new Auto policy before changing behaviour |

The OpenXR loader/SDK and OpenVR declarations must retain their upstream
license notices. Forking OpenComposite would be a different project and
licensing/distribution decision; the implementation plan here is an independent
bridge using public API contracts. Do not base a present-day decision on the
old draft's unrefreshed upstream last-commit date.

## Phase 0: evidence before backend integration

Use existing sanctioned log/build tools. Preserve executable build/hash, DLL
build identities, runtime manifest/name/version, headset, render size, settings
and log timestamps with each capture. Tag calls from Elite separately from
EDVR's own probes and injected clients. "Not observed" is a bounded result, not
proof that a method will never be called.

1. **ABI census and reproducible inventory.** Pin the historical header, record
   all 84 method signatures and the export table, and capture init,
   shutdown/re-init, validity checks and first geometry requests. Instrument
   known exact signatures; do not patch every slot with a generic C thunk. Test
   instrumentation in the existing fake/runtime smoke harness before a flight.
   Capture meaningful arguments for events, properties, controller queries,
   tracking space, hidden mesh, skybox/fades and handoff.
2. **Frame and context ordering.** Record thread IDs and ordered sequences for
   pose wait, both eyes, Present, relevant GPU commands and teardown. Record
   deferred-context execution if present. This decides safe query placement,
   eye capture and the synchronization contract.
3. **Device and texture ownership.** Record every created device, feature
   level/adapter and submitted texture device, including first skybox and eye
   submissions. Capture full descriptors, colour space, bounds and flags at
   first use and changes. Confirm both eye orders and double-wide fixtures in
   desk tests even if only one order is observed in flights.
4. **Monitor prototype and validation.** Build the bracket in the proxy after
   its ownership contract is established. Compare with delayed SteamVR records
   on matched frames; exercise AA/pass settings and post-submit mirror work.
   Phase 0 establishes the instrument; Phase 1 qualifies and ships it.
5. **Runtime capability desk program.** Enumerate extensions, system support,
   adapter requirements, primary-stereo views, blend modes and limits. Session
   tests obtain actual formats, startup geometry, refresh rate and gaze state.
   Run against the actual installed SteamVR, VDXR and Pimax implementations;
   extension enumeration alone does not establish functional support.
6. **Lifecycle and event behaviour.** Exercise headset doff/don, dashboard,
   focus loss, runtime exit, tracking invalidity and recentre. Record the
   events Elite consumes and its response; do not map STOPPING to quit by
   assumption. Test session-loss handling without requiring a game crash.
7. **Optional research.** Sidecar correlation, metrics counters, final-depth
   suitability and direct-output format capabilities get their own findings.
   Their absence must not hold up the baseline backend.

## Phasing and acceptance

| Phase | Deliverable | Gate |
|---|---|---|
| 0 | Census, ownership evidence, bracket prototype, runtime desk harness | Reproducible evidence and safe instrument operation; unresolved first-device/geometry assumptions recorded as blockers |
| 1 | App GPU span and Monitor source/validity changes in proxy | Desk failures covered; SteamVR correlation explained; useful measured values on native-runtime rigs; existing SteamVR counters preserved |
| 2 | Opt-in OpenXR backend with required EDVR features | ABI and fake-XR tests pass; real loader validation; initial geometry, stereo/colour/pose and lifecycle parity on each desk runtime |
| 3 | Field qualification and retirement proposal | Named runtime matrix with results, rollback/install verification and explicit sign-off on exact retired settings/features |

Headset acceptance includes intro/menu/loading, cockpit and terrain edges, FSS
entry/exit, theater/heal, on-foot screen, Explorer Cam, temporal modes and
render-scale changes, settings reload, consecutive withheld frames and normal
shutdown. Repeat relevant cases with asymmetric and canted headset geometry.
Preserve current PP requirements until separately validated. Record frame
cadence, GPU span, image quality and startup duration against the same proxy
configuration; a smooth desk harness is not SteamVR performance parity.

## Implementation work packages after confirmation

Luna agents get bounded ownership and must return changes, tests run and
remaining evidence gaps. The parent agent owns shared contracts, reviews all
diffs and integrates sequentially. Parallel work is limited to independent
files; agents must not race edits to `frame_flag.*`, `build.bat` or config.

1. **Census and ABI fixtures:** exact header/export manifest, safe call census
   and member-ABI tests, including failure buffers and re-init. Start here
   alongside independent Monitor design/desk instrumentation work.
2. **GPU instrument and Monitor:** frame-associated asynchronous queries,
   device ownership, unavailable/stale handling, graphs/overlay/tile sources
   and focused tests. Integrate shared-channel changes through the parent.
3. **OpenXR core:** dispatch interface for a fake runtime, init/device/session
   lifecycle, startup geometry, spaces, events and immutable frame records.
   Begin after the necessary census contracts are reviewed; do not guess past
   an unresolved flight-dependent decision.
4. **Submission and feature integration:** pair state, swapchains/conversion,
   projection and shadow metadata, then extract/adapt existing feature paths.
   This depends on reviewed core contracts and must preserve each branch.
5. **Build and distribution:** pinned dependencies/notices, backend selection,
   both build flavours, installer/rollback and config documentation. Begin
   after artifact and configuration contracts are agreed.

Required desk tests cover incomplete/reversed/duplicate eye pairs,
`shouldRender=false`, invalid tracking, acquire/wait/release failures, missing
formats, resize, recenter, device mismatch/removal, loss/shutdown/re-init and
missing optional extensions. ABI tests use the real historical declarations;
projection/colour tests compare against independent expected outputs. Extend
`tools/fakevr` and `tools/openvr_smoke` where appropriate, and add a fake-XR
call-order harness rather than depending on a headset for failure coverage.

Every C++ change must pass `build.bat` by absolute path and the relevant
existing gates. New Python tools have `--self-test` and enter the build gate;
config changes pass `tools/check_config_contract.py`, exit changes pass the
existing exit-path checks. The parent reviews resource lifetimes, frame/pose
semantics and claimed test coverage before integration. If a gate needs a
flight, prepare the exact instrumented build and capture instructions and
report that remaining requirement explicitly.
