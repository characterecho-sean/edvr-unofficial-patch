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

## Validation and next headset gate

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
fresh native gate remains pending. Earlier native receipts do not qualify the
changed executable. Clearing during idle loading is currently qualified by
desktop frame/policy tests; this native scene exercises override retirement
after its first completed game pair.

For the next 20-second PiOpenXR run, close Frontier and SteamVR and obtain
fresh readiness with the Pimax worn. The scene begins with roughly three
seconds of a colored grid surrounding the viewer, then switches to the normal
triangle. The grid is a diagnostic texture pattern, not an implementation of
SteamVR's compositor grid. Caller skybox textures are overwritten and released
before the viewing interval. Both views must stay upright and stable during
head rotation, the transition must look normal, and the test must close
normally.

The receipt must show loading projection frames with zero game Submits and an
unchanged game pose cache, followed by one transition to scene rendering and
explicit override retirement. Game waits still equal main-loop frames plus
startup frames. Total XR frames additionally include separately counted loading
frames. Eye copies/Submits remain twice the completed game stereo pairs;
loading must not inflate those counts. Normal System-thread Shutdown, owner
join, reset events and resource cleanup must continue to pass.
