# OpenXR six-face skybox and loading checkpoint

This checkpoint follows the passed owner-thread headset gate. It adds private
six-face skybox capture, a native-FOV loading renderer and explicit loading
frame ownership to the standalone diagnostic. Frontier's installed pair remains
`f3c205e`; the new transport is still absent from shipping discovery.

## Evidence and supported form

The matching Frontier log `edvr_vr_20260912_101849.log`, read through
`edvr_log.py --expect-build f3c205e`, contains three successful game-origin
`IVRCompositor_014::SetSkyboxOverride` calls. Each supplied six textures. Only
the first face's DirectX/Auto/non-null metadata was recorded. Dimensions and
all six resource descriptors remain unmeasured. The earlier 1x1 observation
described dimensions, not a one-element skybox array. `early_session.cpp`
itself performs a separate one-pixel left-eye Submit.

The pinned [Valve v0.9.20
header](https://raw.githubusercontent.com/ValveSoftware/openvr/v0.9.20/headers/openvr.h)
defines the six-face order. Face orientations are derived from Valve's [SteamVR
skybox snapshot
cameras](https://github.com/ValveSoftware/steamvr_unity_plugin/blob/master/Assets/SteamVR/Editor/SteamVR_SkyboxEditor.cs),
read at blob `3f94064f6bf438c81df0399d159dc133d3bf60cd`. Converting Unity's
forward Z to OpenVR/OpenXR's negative Z gives:

| Face | Forward | Image up | Image right |
| --- | --- | --- | --- |
| Front | -Z | +Y | +X |
| Back | +Z | +Y | -X |
| Left | -X | +Y | -Z |
| Right | +X | +Y | +Z |
| Top | +Y | +Z | +X |
| Bottom | -Y | -Z | +X |

This is the implemented six-face contract, not a measurement of Frontier's face
contents. The header's one- and two-texture lat-long forms remain unavailable
and return a conservative historical compositor error.

## Ownership and rendering

`SkyboxCapture` validates every source and allocates six fresh private textures
before copying any pixels. A rejected update preserves all previously published
faces. It accepts independent face dimensions, including 1x1 and non-square
textures, RGBA/BGRA typeless/UNORM/sRGB families and Auto/Gamma/Linear
metadata. Sources must be live same-device D3D11 Texture2D objects with one
mip, one array slice and one sample. Invalid COM type, wrong device and
unsupported format remain distinct errors. Caller textures can be overwritten
and released after the synchronous call returns.

The owner serializes immediate-context copies, rendering and cleanup. The
diagnostic renderer samples all six private faces into its existing pair of
OpenXR projection swapchains; it needs no optional cube-layer extension. Exact
native eye orientation and asymmetric FOV determine the ray. Translation does
not move this infinite-distance background, while submitted layer metadata
retains the original eye poses. Hardware sRGB views decode Auto/Gamma before
linear filtering; Linear faces use UNORM views. Output uses sRGB targets or
explicit sRGB encoding for UNORM targets. Filtering clamps within each face;
seamless filtering across cube edges is not implemented.

Every source and both views are checked before the first swapchain acquire.
Each acquired image must be successfully waited before drawing and releasing. A
timeout is not permission to draw or release; uncertain operations retire the
renderer without retries. This still owns the diagnostic context and uses
`ClearState`; preserving a live game's graphics state remains separate work.

The subsequent [context-preservation
checkpoint](openxr-context-state-2026-09-12.md) moves this rendering into
private deferred command lists. The observations and native receipt below
describe the earlier skybox executable.

## Loading ownership

An accepted override before the first successful scene Submit activates startup
loading. Later replacements update the stored override without interrupting an
active scene. `ClearLastSubmittedFrame` closes a pending frame and activates
the available override. The historical default compositor grid remains
unavailable when there is no override.

The event pump can advance loading only when no game frame is open and no API
job is queued. Each loading frame has its own wait/begin, predicted geometry,
projection or zero-layer end. Missing tracking or `shouldRender=false` skips
the rendering work. Loading locates do not replace the cached render/gameplay
poses returned by the game's `GetLastPoses` calls. Natural origin changes still
invalidate those caches through the existing origin policy.

The first accepted scene Submit leaves loading mode. Clearing a visible
override schedules one empty frame when the owner is idle; a game frame takes
priority. Clear/shutdown release retained faces. Fades, grid rendering,
suspension, unobserved lat-long forms and a policy for arbitrary missed game
deadlines remain explicit follow-up work.

## Validation and headset gate

Luna implemented the initial capture and renderer. Parent review corrected
transaction boundaries, shader packing, rotation and UV signs, color decoding,
pipeline setup, source-format rejection and incomplete test coverage. The
original WARP regressions were retained. A nested fake-runtime fixture lifetime
caused a desktop test crash; fixture lifetimes were separated before rerunning.
No native headset session used those failing revisions.

Desktop tests exercise copied pixel lifetime, mixed formats/colors/dimensions,
late-face rejection, replacement and retirement. Renderer tests compare actual
WARP pixels with an independent quaternion-vector and face-plane oracle,
including all six directions, off-center samples, asymmetric FOV, cant,
translation invariance and both output formats. Injected swapchain failures
exercise both eyes and timeouts. ABI and frame tests cover source dispatch,
generation rejection, partial game frames and loading failure closure.

The complete absolute-path `build.bat` passed with the installed Frontier
original OpenVR DLL as its export input. Results include 150 frame/loading
checks, 219 compositor checks, 74 skybox capture checks, 5240 stereo checks, 62
owner-service checks, 28 native self-tests and the 252-key config contract. The
final log is `build/openxr-skybox-final-build.log`; executable, source, loader
and manifest hashes are retained in `build/openxr-skybox-validation.json`. The
fresh native API and user visual gates passed as recorded below. Earlier native
receipts did not qualify the changed executable. Clearing during idle loading
is currently qualified by desktop frame/policy tests; this native scene
exercises override retirement after its first completed game pair.

For this 20-second PiOpenXR gate, close Frontier and SteamVR and obtain fresh
readiness with the Pimax worn. The scene begins with roughly three seconds of a
colored grid surrounding the viewer, then switches to the normal triangle. The
grid is a diagnostic texture pattern, not an implementation of SteamVR's
compositor grid. Caller skybox textures are overwritten and released before the
viewing interval. Both views must stay upright and stable during head rotation,
the transition must look normal, and the test must close normally.

The receipt must show loading projection frames with zero game Submits and an
unchanged game pose cache, followed by one transition to scene rendering and
explicit override retirement. Game waits still equal main-loop frames plus
startup frames. Total XR frames additionally include separately counted loading
frames. Eye copies/Submits remain twice the completed game stereo pairs;
loading must not inflate those counts. Normal System-thread Shutdown, owner
join, reset events and resource cleanup must continue to pass.

## Native result and limits

The `612d73e` executable completed the freshly authorized PiOpenXR run on
2026-09-12. Its executable, all 74 source inputs, loader and runtime manifest
matched the final-build record. Pimax OpenXR 0.1.0 reported D3D11.1, 5424 x
5356 per eye and sRGB swapchain format 29. The receipt records exit 0 after
21.016 seconds, with no watchdog timeout. Frontier and SteamVR were absent at
preflight and at the post-test process observation.

The owner rendered 223 skybox projection frames while the application made no
Submit calls. Its only prior compositor wait was the startup geometry frame;
the game pose cache remained unchanged throughout the loading interval. The
first accepted scene Submit produced exactly one transition from loading, and
the override was cleared after the first completed game pair. All private
skybox textures retired. This run did not exercise the deferred empty frame for
clearing during idle loading (`clear_frames=0`).

The subsequent scene completed 1531 main-loop frames: 1529 stereo pairs and two
zero-layer frames. Compositor waits were 1532, including the startup frame;
cached comparisons were 1531. There were 3058 Submit calls and private eye
copies, and 1529 handoffs/compositions. Loading did not inflate those game
counters. All 1530 sampled scene views and head poses were valid.

Init, System and owner thread IDs were distinct (34544, 23068 and 39408). Both
seated resets invalidated caches and delivered their reset event, with zero
position error and yaw errors below 0.000001 radians. There were 1815 event
pumps and 1850 live System queries, of which 1849 returned valid poses; these
counters do not establish the cause of the one invalid query. Shutdown ran on
the System thread, joined the owner, retired interfaces, advanced the token to
2 and cleaned resources once. No natural runtime-origin change was observed.

The user confirmed that the colored grid appeared in both eyes, stayed upright
and fixed during head turns, transitioned normally to the triangle, and that
the triangle looked and tracked normally and the test closed normally. This
completes the standalone loading-to-scene visual and normal-lifecycle gate. It
is not a frame-timing or native Frontier qualification.

The receipt, output and post-test process observation are under
`build/openxr-native-20260912-184533`; parsed counters and visual confirmation
are retained in `build/openxr-skybox-validation.json`. Live game-device
ownership, graphics-state preservation, complete legacy exports, the paired
feature handshake and remaining compositor features still need implementation
and qualification before native Frontier launch.
