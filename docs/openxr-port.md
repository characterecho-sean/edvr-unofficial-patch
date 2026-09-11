# Elite on OpenXR: a drop-in `openvr_api.dll` that speaks OpenXR

*A design document, written before the code (2026-09-11). Claims about EDVR
cite the source; claims about the game and the runtimes are labelled measured
(this repo's logs, the game's binaries, field reports), vendor-stated (specs,
headers, published source) or believed; what can only be settled at
implementation time or in a live session is collected under Phase 0. Nothing
here is implemented.*

## The ask

Today EDVR's `openvr_api.dll` is a proxy: it forwards every export to the real
runtime DLL underneath (Valve's, or OpenComposite's) and patches two vtables in
place to reach the frame boundary and the submit
(`src/openvr/openvr_proxy.cpp`, `compositor_hook.cpp`, `system_hook.cpp`). The
proposal is to stop forwarding. EDVR's file becomes the OpenVR runtime as far
as Elite is concerned, implements the interfaces the game asks for, and speaks
OpenXR to whatever runtime the machine has. Elite keeps calling OpenVR and
nothing in the game changes.

Two requirements come with it:

1. **OpenXR exclusively.** No SteamVR-native forwarding path is kept once the
   layer is trusted. SteamVR is itself an OpenXR runtime, so SteamVR users are
   served through it.
2. **The Monitor page keeps its GPU figures.** OpenComposite never provided
   them, and OpenXR has no compositor-timing call at all, so the design has to
   find them elsewhere. It does -- see "The Monitor page's GPU figures" below,
   which is also the first thing to build.

## Where this stands against the 2026-09-01 goal

The goal stated on 2026-09-01, during the render-scale flights, was to support
native SteamVR only and drop OpenComposite, because much of EDVR's
transport-safety machinery exists for OpenComposite-over-VDXR failures: the
cull guard's two-stage go-live and canonical-size snap, the copy-not-bounds
crop (`guard_crop.h`), the early handover (`early_session.cpp`), the launch
centre (`launch_centre.cpp`).

This design does not reverse that goal; it reaches it by the other road. It
drops OpenComposite by replacing it, instead of by walking away from the
native-runtime population -- which is where both field rigs live (a Quest 3
over Virtual Desktop and a Pimax Crystal Super over PiOpenXR, both through
OpenComposite: README, "Field-verified") and where most reports come from. What
the goal had right still stands: the tolerance machinery exists because
OpenComposite has to guess things, and a layer that does not guess does not
need it.

## What Elite needs from `openvr_api.dll` (measured)

How it was measured, 2026-09-11: the delay-load import table of
`EliteDangerous64.exe` in the Steam install, parsed with a stdlib PE reader in
the manner of `tools/gen_exports.py`; the `IVR*_nnn` literals from a string
scan of the same binary (also recorded at the top of `compositor_hook.cpp`);
the export tables of the game's `openvr_api_orig.dll` and of EDVR's proxy; and
the `VR_GetGenericInterface("...")` lines the proxy writes, in the three newest
VR logs across the two installs (Frontier 2026-09-10 19:51 and 20:06, Steam
2026-09-11 07:42).

| What | Measured |
|---|---|
| How the DLL is loaded | delay-load import: mapped at the first call, about 1.2 s after the game's device exists, and never at all on the Oculus path |
| Exports imported | 5: `VR_InitInternal`, `VR_ShutdownInternal`, `VR_GetGenericInterface`, `VR_IsInterfaceVersionValid`, `VR_GetInitToken` |
| Exports in the file | 14 in Valve's: the five above, `VR_IsHmdPresent`, `VR_IsRuntimeInstalled`, `VR_RuntimePath`, three error-string calls, `VRControlPanel`, `VRDashboardManager`, `VRTrackedCamera`. 16 in EDVR's, with two self-test exports |
| Interface literals in the binary | `IVRSystem_012`, `IVRCompositor_014`, `IVRChaperone_003`, `IVRExtendedDisplay_001`, `IVROverlay_011` |
| Interfaces requested in a session, in order | `IVRSystem_012` at t=0; `IVRExtendedDisplay_001` within 15 ms; `IVRCompositor_014` 2 to 2.5 s later; `IVRChaperone_003` up to 3 s after that |
| Never requested in any log | `IVROverlay_011`. The one `IVROverlay_028` request on record came from something injected, not the game (`openvr_proxy.cpp`) |
| Compositor re-requests per session | 2 to 3, each a few seconds before the eye textures change size ("ONE EYE ... CHANGED") |
| Methods across the four requested interfaces, per the openvr 0.9.20 header | about 85 (exact count from the header at implementation); the proxy hooks or calls 8 of them |
| System calls per frame | `GetRecommendedRenderTargetSize`, `GetProjectionMatrix`, `GetProjectionRaw`, `GetEyeToHeadTransform`, each about 12 times a frame (~1080/s at 90 Hz: `system_hook.cpp`) |
| Compositor calls per frame | `WaitGetPoses` once, `Submit` twice; `SetSkyboxOverride` once at startup with a 1x1 texture (OpenComposite's log, `early_session.cpp`) |
| Projection planes asked for | 0.025..50000 for the scene and 0.1..1000 for something else (`system_hook.cpp`) |
| Tracking space | seated (the launch centre's reset reached it) |
| Eye-to-head rotation | dropped by the game (docs/canted-projection.md) |
| Submit shape | one texture per eye, null bounds, on both rigs now; one double-wide texture with per-eye bounds on a Quest 3 over Steam Link, 2026-08-17 (`noteEyeTextureSize`) |
| Submit formats seen | the 8-bit RGBA and BGRA family: typeless, UNORM and sRGB (`supersample_resolve.cpp`) |
| Threads | Submit and Present on one thread (measured 2026-08-15); VR init off the render thread (measured 2026-08-29); WaitGetPoses' thread not yet logged |
| The environment on the two installs, latest logs | Steam: Valve's DLL, 4536x4480 recommended per eye. Frontier: OpenComposite, 5424x5356 recommended per eye, and "the recentre DID NOT TAKE" |

Two consequences worth stating up front. The replacement keeps every one of the
14 export names (delay-load binds by name, and other clients may ask for the
rest), answers `VR_IsInterfaceVersionValid` true for exactly the five literals,
and hands back the same interface object on every re-request. And
`IVRSystem_012` is the generation whose `GetProjectionMatrix` takes a fourth
parameter and returns a 4x4 by value: implemented as a C++ class compiled
against that generation's header, the by-value return and the hidden `this` are
the compiler's business, which is the whole difference from hooking.

## What the VR half does at those seams today

Everything below keeps its seam under the layer; it is listed so nothing is
lost in the move.

At the frame boundary (`hookedWaitGetPoses`): the launch centre; the gaze probe
and the gaze source; the compositor timing read; the pacing statistics;
clearing the glitch mark and the submit-pair latch; the transition hold; the
pose ring and the pose hold; the ship-forward publication; the head offset
(Explorer Cam); the theater's freeze; the cull guard's stage transitions and
the temporal jitter (`systemHookFrameBoundary`); the wait time for the Monitor.

At the door (`hookedSubmit`, in order): the temporal pass, the guard crop, the
supersample resolve, the sharpen, the menu; the theater and the eye heal
substitutions; the FSS arrival-mono swap; the transition-flash withhold with
the resubmit shadow; the eye-size and bounds publication;
`Submit_TextureWithPose` for the pose hold; the door GPU bracket.

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
checks, the convention-capture thunks in `system_thunks.asm`, and the 6c rule.
None of it carries over. That is the size of the simplification, and also the
size of the safety net being replaced.

## Pros

- **EDVR owns the seams instead of patching them.** The pose wait, the submit
  and the projection answers become EDVR's own implementation. The whole hook
  scaffolding above exists because the VR half patches someone else's objects;
  a class compiled against the exact 0.9.20 header makes by-value returns and
  the hidden `this` the compiler's problem.
- **One source of truth for the frustum.** Tangents, the 4x4 and eye-to-head
  all derive from one OpenXR field-of-view and one view pose. The cull guard,
  the temporal jitter and the canted-panel fold (docs/canted-projection.md)
  become edits to one struct, and the formula check and the never-mixed-answers
  rule become structural rather than policed.
- **The OpenComposite bug class dies at the root.** The session rebuild that
  stalled the intro movie, the temp session behind the yaw-180, the origin that
  lands somewhere new each launch, the 2026-09-10 "recentre DID NOT TAKE", the
  ignored submit bounds (commit 8c55791, the reason the guard crops by
  copying), the fabricated frame timing
  (docs/review-opencomposite-startup-2026-09-10.md) and the fatal dialog for an
  unknown interface all come from OpenComposite guessing the game's device and
  rebuilding, or from it filling what it cannot know. The d3d11 half publishes
  the game's device before the first VR call, and the early handover proved
  that device is the one the game submits from (the rebuild moved, 2513 ms to
  22 ms, `early_session.cpp`). The layer can create one session on the right
  device and never rebuild.
- **Performance for the non-Valve majority.** PimaxXR, VDXR and Varjo get the
  frame without the SteamVR compositor hop, which is the reason people install
  OpenComposite at all. Valve-runtime users should see no change, because
  SteamVR is itself an OpenXR runtime; that parity is a Phase 2 measurement,
  not an assumption.
- **The render pose travels with the frame.** An OpenXR projection layer
  carries the pose each eye was rendered from (vendor-stated:
  `XrCompositionLayerProjectionView::pose`). The pose-hold instrument, the
  theater's world lock and any withheld frame tell the compositor the truth by
  construction, instead of through a flag OpenVR makes optional and a
  translation layer may not honour.
- **Standard eye tracking and other extensions.** `XR_EXT_eye_gaze_interaction`
  in a session EDVR owns, which reaches the focused state, on the Pimax runtime
  that already implements it properly (docs/eye-tracking.md, "Two things noted
  on the way"); the driver frame repair becomes unnecessary there. Depth layers
  from the depth the d3d11 half already has, a quad layer for the settings
  menu, the refresh rate and the visibility mask all become available where a
  runtime offers them.
- **One copy fewer at the door when a pass is on.** Today the last EDVR pass
  writes an EDVR texture and the runtime copies it again. Under OpenXR the last
  pass writes straight into the acquired swapchain image. With no pass on, the
  runtime's copy simply becomes EDVR's, at the same cost.
- **Runtime choice under EDVR's control.** Setting the runtime path before
  instance creation gives an ini key that picks SteamVR, Virtual Desktop or
  Pimax per install, with no system-wide switch. OpenXR API layers reach Elite
  on SteamVR rigs for the first time.
- **Licensing stays clean.** The OpenXR loader and headers are Apache-2.0 and
  BSD-3, so EDVR stays MIT. OpenComposite is GPL-3.0-or-later, so nothing can
  be lifted from it either way, only re-derived from the spec.

## Cons

- **The environment matrix grows.** Two transports today (Valve's DLL, and
  OpenComposite over anything) become one layer over SteamVR's OpenXR, VDXR,
  PimaxXR and Varjo, and every runtime's quirk becomes an EDVR bug report.
  Three of those can be flown from the desk; Varjo is field-only. Meta's own
  runtime is mostly out of reach anyway, because Elite prefers its Oculus path
  whenever the Meta app is running (README, "Does not work").
- **The session state machine is where OpenComposite spent years.** Headset
  doffed, dashboard open, runtime restarted, resolution changed under the game
  (Elite re-requests the compositor when its targets change, measured above),
  and the two D3D11 devices Elite creates at startup (feature level 12.0 then
  11.0, docs/review-opencomposite-startup-2026-09-10.md), where binding the
  wrong one is the DEVICE_REMOVED class from issue 21. Elite's render loop
  never stops, so the pose wait must keep answering through every session
  state. Each of these is a flight per runtime.
- **Some OpenVR semantics have no OpenXR equivalent and must be emulated.** The
  seated recentre becomes recreating the local space with an offset, plus
  handling recentres the runtime does on its own. Compositor frame timing has
  no counterpart at all, so the Monitor's compositor columns go blank
  everywhere -- including on SteamVR, where they work today
  (`frame_timing.cpp`) -- unless a source in the Monitor section below exists.
  Events must be synthesised from session states, and how Elite reacts to a
  quit or a focus loss decides what happens when a runtime goes away.
- **The frame loop translation is a new place to be subtly wrong.** One
  blocking call becomes wait, begin and locate, and two submits become one
  end-frame. Elite runs VR init off the render thread and submits and presents
  on one thread, and the layer must enforce OpenXR's ordering across that. The
  colour-space rule must copy OpenVR's exactly (vendor-stated, `openvr.h`
  `EColorSpace`: Auto means gamma for 8-bit formats and linear otherwise) or
  the washed-out class comes back. Both submit shapes in the table must work.
- **Transition-flash withholding changes shape.** Swapchain images rotate, so
  re-showing the previous frame means copying the shadow into the acquired
  image. The copy exists today (`resubmit_shadow.cpp`), but the
  second-withheld-frame logic of docs/transition-flash.md needs
  re-verification.
- **Scope is bounded, verification is not.** A single-game layer over D3D11
  with no controllers, overlays or actions is plausibly a few thousand lines,
  against the VR half's 8,945 today. Flights are the binding resource on this
  project, and a layer with no fallback ships with every runtime's quirks
  unflown.
- **Keeping the proxy path as a fallback doubles the surface.** Both code paths
  stay alive, the hook scaffolding and the layer, for as long as the fallback
  exists.
- **Nothing changes for the Oculus-native population.** Elite on LibOVR never
  loads this file (docs/troubleshooting.md, `vr_runtime.cpp`), and the README
  should keep saying so.
- **Deployment gains a moving part.** The OpenXR loader, statically linked or
  as a third file; the registry-selected active runtime as a new support
  question; and Defender's history with new EDVR binaries.

## The layer, sketched

What follows is the shape, not the code. Each item names the OpenVR call it
answers and the OpenXR call it stands on; "believed" marks the ones Phase 0 has
to confirm.

**Lifecycle.** `VR_InitInternal` creates the instance (loader statically
linked, `XR_KHR_D3D11_enable` required), gets the system, checks
`xrGetD3D11GraphicsRequirementsKHR` against the game's adapter, and creates ONE
session on the device the d3d11 half has already published (`gameDevice()`, the
`_v20` field of the shared block) -- so the session exists on the right device
before the game's first interface request, and there is never a temp session to
rebuild. This makes `d3d11.dll` a requirement of the VR half, which the
settings menu already is; an openvr-only install is a decision below. Session
states: begin on READY, end on STOPPING, and while the session is not running
the pose wait answers with the last pose and runs no frame loop, because
Elite's loop never stops. Instance loss becomes `VREvent_Quit`.
`VR_ShutdownInternal` tears it all down; Elite is known to re-request
interfaces mid-session but has not been seen to re-init (believed; the census
below settles it).

**The frame loop.** `WaitGetPoses` is: end the previous frame if it is still
open, `xrWaitFrame`, `xrBeginFrame`, `xrLocateViews` at the predicted display
time in the seated space, and from that the HMD pose (device 0) and the two eye
poses. `Submit(left)` stores; `Submit(right)` acquires, waits, copies (the
bounds become a sub-rectangle, and a v that runs backwards a flip), releases
both images and calls `xrEndFrame` with one projection layer whose per-view
pose is the pose the game rendered from -- the frozen one when EDVR fed it one.
`PostPresentHandoff` is a no-op. A frame that submits one eye or none ends with
what it has; two waits without a submit end the open frame with no layers
first. Ordering is enforced by the layer, and the wait's thread is Phase 0 item
2.

**Projection.** One `XrFovf` per eye from `xrLocateViews` is the single source:
`GetProjectionRaw` returns its tangents in OpenVR's order (`pfTop` is the
physical -Y edge, the reversal docs/settings-menu.md records);
`GetProjectionMatrix` composes the DirectX-convention 4x4 with the game's
planes; the guard's lie and the temporal jitter are applied to the one struct
before either is answered. `GetEyeToHeadTransform` is the eye pose relative to
the view space, rotation included, which is where the canted fold of
docs/canted-projection.md would be applied. `GetRecommendedRenderTargetSize` is
the recommended view size, multiplied by the guard and scale factors exactly as
today.

**Colour.** The swapchain format follows OpenVR's own Auto rule: an 8-bit UNORM
or typeless submit gets an sRGB-typed swapchain, everything else a linear one,
chosen from `xrEnumerateSwapchainFormats`. A copy between UNORM and UNORM_SRGB
of the same family is a bitwise copy, so the compositor sees the bits Elite
wrote, interpreted as SteamVR interprets them.

**Spaces and recentre.** The seated universe is the LOCAL reference space.
`ResetSeatedZeroPose` recreates it with the current head yaw and position as
its origin, which is what OpenComposite does too. A
`REFERENCE_SPACE_CHANGE_PENDING` event is a runtime-side recentre, the pose
ring's "moved and STAYED" case. The launch centre becomes internal and
runtime-agnostic: the layer centres on the first valid pose itself, and the
export-based runtime test is retired.

**Events.** `PollNextEvent` drains a queue the layer fills from session states:
focused and visible map to input-focus captured and released (believed:
dashboard events too); stopping, exiting and instance loss map to
`VREvent_Quit`; an eye-to-head change maps to `VREvent_IpdChanged`. Which of
these Elite consumes, and what it does on a quit, is Phase 0.

**Stubs.** `IVRExtendedDisplay_001` reports no extended mode.
`IVRChaperone_003` reports calibrated, with the play area from the stage bounds
where the runtime has them and zero otherwise. `IVROverlay_011` is answered
"interface not found", which is what OpenComposite answers and what the game
has never been seen to ask for. Everything else on the four interfaces that the
census finds uncalled returns the documented failure value, and a first call to
any of them is logged once, so the stub list is checked by the field rather
than trusted.

**The withhold.** The resubmit shadow is copied into the acquired image instead
of being handed over as a texture; the rest of the transition-flash machinery
keeps its seam.

**Runtime selection.** A `[vr]` key, final name subject to the ini rule that
values name what the player gets -- `runtime = auto | steamvr |
virtual_desktop | pimax` -- sets the runtime path before instance creation;
`auto` is the registry's active runtime. One line at startup names the runtime
and its version from `xrGetInstanceProperties`, replacing the export sniff.

**Extensions used where present** (vendor-stated names): the
performance-counter time conversion, for correlating predicted display times
with EDVR's own clocks; the depth layer (Phase 2 option, and whether an
infinite-far reversed-Z depth can be described is Phase 0); the visibility mask
for `GetHiddenAreaMesh`; the refresh-rate extension; eye gaze; and the Meta
performance-metrics extension for the Monitor. Each is optional, announced
once, and its absence is not an error.

## The Monitor page's GPU figures

Every GPU tile on the Monitor page today comes from the compositor's own
record, `IVRCompositor::GetFrameTiming`, read once a frame by
`frame_timing.cpp` and published on the channel; `perf_monitor.h` lists each
tile's source. OpenComposite fills that record with constants, so the tiles are
blank there, and OpenXR has no equivalent call, so the layer would leave them
blank too. The number those tiles exist for -- the game's GPU frame time
against the display budget, which is what every settings trade turns on -- does
not have to come from the runtime, and should not. The half that owns the
immediate context can measure it, on every transport, and it already does so
for EDVR's own passes: the door bracket (`edvrDoorGpuBegin` / `edvrDoorGpuEnd`,
`perf_monitor.cpp`) is a timestamp pair on the game's context, never awaited,
polled on later frames. Widening that bracket to the whole frame is the fix, it
ships in the current proxy before any OpenXR code exists, and the layer
inherits it unchanged.

| Tile | Today, SteamVR | Today, OpenComposite | Under the layer |
|---|---|---|---|
| App GPU | compositor's record | blank | EDVR's frame bracket: first GPU command after the boundary to the end of the swapchain copy |
| EDVR at the door | door bracket | door bracket | door bracket, extended to the swapchain copy |
| GPU time, total | compositor's record | blank | app GPU against the display budget; the compositor's cost n/a unless a source below exists |
| Compositor GPU | compositor's record | blank | SteamVR sidecar where the runtime is SteamVR; the Meta metrics extension where offered; otherwise "n/a on this runtime" |
| Dropped | compositor's count | blank | inferred: the predicted display time stepping by more than one display period |
| Reprojected | compositor's flags | blank | inferred: a sustained two-period cadence while the app delivers every period |
| Display Hz | device property | device property | the predicted display period; the refresh-rate extension where offered |
| CPU time, wait, Present | EDVR's own clocks | EDVR's own | unchanged; the wait becomes the block inside `xrWaitFrame` |
| GPU load, temperature | NvAPI, NVIDIA only | same | same, plus a per-process GPU engine counter from PDH as the vendor-neutral fallback |

**Where the frame bracket's stamps go.**

- The **begin** stamp is issued at the first hooked GPU command after the frame
  boundary, by the d3d11 half, on whatever thread issues that command. That is
  thread-safe by construction and assumes nothing about which thread calls the
  pose wait. It also mirrors the compositor's own definition, which starts
  counting at the app's first work after running start rather than at the
  previous present, so a light GPU reads well under budget instead of at it.
- The **door** stamp is where the existing bracket begins, so the frame splits
  into the game's part and EDVR's part, which is the split the settings rows
  need.
- The **end** stamp lands after the last door pass -- under the layer, the copy
  into the acquired swapchain image -- on the submit thread, which is measured
  to be the Present thread. One disjoint query wraps all three; the existing
  query ring and its polling loop are reused.
- **A span, not busy time.** A timestamp pair measures when the GPU started and
  finished, bubbles included, which is also what the compositor's figure means.
  The GPU load tile beside it is what tells a CPU-bound frame from a GPU-bound
  one: the pairing docs/performance.md's foveation flights needed and lacked
  for three flights.

**Validation.** One flight on the Steam install, with the current proxy, prints
the compositor's scene and total GPU figures beside EDVR's bracket for the same
frames. Agreement licenses the bracket on VDXR and PimaxXR, where there is no
reference at all. The flight rides on any other test.

**What stays runtime-dependent, and how each is handled.**

- The compositor's own cost runs in another process and cannot be
  self-measured. Where the runtime is SteamVR, a second OpenVR init as a
  background application from inside the process reads the same record fpsVR
  reads (believed; two clients in one process on top of SteamVR's OpenXR
  runtime is a desk test before it is a design). Where a runtime offers the
  Meta performance-metrics extension, take it. Elsewhere the tile says "n/a on
  this runtime", and the page leads with app GPU against budget.
- Drops become inference. The layer is handed the predicted display time every
  frame; a step of two periods is a missed interval, and a sustained two-period
  cadence is the runtime synthesising frames. Standard practice for OpenXR
  applications, and exact from the layer's position, but not the compositor's
  own count: the tile's caption becomes "late" rather than "dropped".
- The GPU load tile is NvAPI today; a per-process GPU engine counter from PDH
  gives the same reading on any vendor at a once-a-second sample, and belongs
  beside it.

The bracket is the first thing to build, in the current proxy: it is the only
part of this design that both populations need today, and the port inherits it
for free.

## Decisions the design has to settle

1. **Migration shape.** A build flavour, or a developer-tier key
   (`advanced.vr_transport = layer | passthrough`, name subject to the ini
   rule), with the proxy path kept for a fixed number of releases and then
   deleted together with its scaffolding. Not a permanent dual path.
2. **The device policy.** Require `d3d11.dll` and bind the session to the
   published device at the first interface request. An openvr-only install is
   either unsupported, and says so in one line, or gets an OpenComposite-style
   temp session; the first is recommended.
3. **The Phase 0 census before any new code** (below): which of the roughly 85
   methods Elite calls, how often, with what arguments.
4. **The runtime matrix with a name against each row**: SteamVR's OpenXR (Steam
   Link, Pimax Play's SteamVR mode, lighthouse headsets), VDXR, PimaxXR, Varjo,
   and WMR recorded as retired by Microsoft.
5. **The retired keys and their replacements**: `advanced.real_openvr_dll`
   chaining, `advanced.suppress_interfaces`, the launch centre's export-based
   runtime detection, `advanced.compositor_timing`, the handover's
   `fix.vr_handover`.
6. **Alternatives rejected in writing** (next section).

## Alternatives rejected

- **Fork OpenComposite.** GPL-3.0-or-later against EDVR's MIT; a codebase many
  times the size of this layer, covering every OpenVR generation, Vulkan,
  OpenGL, input actions and overlays; an upstream whose last commit on the
  OpenXR branch is 2025-07-21 (reviewed 2026-08-29, `early_session.h`); and
  EDVR would still be hooking it rather than owning the seam. The 2026-08-29
  question, "fork or not", was answered "not yet, fix it from our side"; the
  list of things fixed from our side since is the pros section above, and it is
  why the answer changes.
- **The SteamVR-only goal as stated.** It leaves the native-runtime users on a
  dead layer, and both field rigs are among them.
- **A permanent dual path.** Every fix would be written twice or apply to one
  population only; the hook scaffolding would never be deleted.

## Phase 0: measure before building

Each item names what to log and what the answer decides. Items 1 to 3 and 7
extend the current proxy and cost no new DLL.

1. **The slot census.** Count every vtable slot on all four requested
   interfaces (the proxy already patches in place; counting thunks on the
   remaining slots is the same mechanism), and for the calls that carry
   arguments worth knowing, their values: events polled and which event types
   the runtime delivers, controller state, the hidden area mesh, the skybox,
   fades, `PostPresentHandoff`, `SetTrackingSpace`. One flight per desk
   runtime. This turns "about 85, 8 used" into an implemented list and a stub
   list with evidence.
2. **The pose wait's thread id**, one log line beside the submit thread's
   (`compositor_hook.cpp` already logs the latter). Decides whether the begin
   stamp and the frame-loop calls can sit in the layer's own pose wait or must
   ride the draw hooks.
3. **The frame bracket validated against the compositor's record** on the Steam
   install, one flight, as described above.
4. **The extension lists of the three desk runtimes** (SteamVR, VDXR, PimaxXR),
   from `xrEnumerateInstanceExtensionProperties` in a small desk program. No
   flight. Answers the metrics, refresh-rate, eye-gaze, visibility-mask and
   depth rows.
5. **The SteamVR sidecar desk test**: a background OpenVR init in a process
   that also holds an OpenXR session on SteamVR, and whether `GetFrameTiming`
   answers for the scene application. Decides the compositor-GPU tile on
   SteamVR.
6. **Which device the publication picks** when the game creates two: log the
   published device at publication and the submitted texture's device at the
   first submit, and confirm they agree (the early handover's result says they
   do, on one rig).
7. **The submitted formats per rig**, by adding the format to the "ONE EYE is"
   line. Decides the swapchain format table.
8. **How Elite reacts to a quit and to focus loss**, from the events the
   runtime delivers today (item 1) and the game's behaviour after them. Decides
   the event mapping.
9. **Whether a reversed-Z, infinite-far depth can be described to the depth
   layer extension** (vendor-stated at implementation, from the spec text).
   Decides whether depth submission is an option at all.

## Phasing

- **Phase 0**: the measurements above, in the current proxy, one flight per
  desk runtime plus the desk programs.
- **Phase 1**: the frame GPU bracket and the Monitor changes, in the current
  proxy. Ships to everyone; fixes OpenComposite's blank tiles without waiting
  for the port.
- **Phase 2**: the layer behind the migration key, on the three desk runtimes,
  with the census-derived implemented and stub lists, the SteamVR parity
  measurement, and the transition-flash re-verification.
- **Phase 3**: the field (Varjo, and Meta's runtime where it is reachable),
  then the proxy path retired at a named release and its scaffolding deleted.
