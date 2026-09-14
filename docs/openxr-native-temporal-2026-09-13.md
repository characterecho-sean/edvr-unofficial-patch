# Native temporal eye processing

This checkpoint connects native OpenXR submission to EDVR's existing temporal
filter and NVIDIA DLAA/DLSS path. It follows the qualified manual startup and
menu checkpoint. In-game image quality and projection-query ordering still
require the next Frontier flight.

## Frame and graphics contract

`edvrAcquireNativeTemporal` is a separate, versioned capability. It does not
announce support for legacy glitch withholding or other unported effects. The
provider retains bounded CPU contexts; the native host retains the graphics
module and device until callbacks are retired.

At a successful render-frame wait, the XR owner publishes the exact head pose,
eye-to-head transforms, asymmetric frusta, runtime/reference generations and
sequence to the provider. This callback performs CPU work only. The provider
freezes settings previously read on the game producer and returns per-eye
tangent jitter derived from observed input dimensions. Both game-facing raw
projection and matrix queries read the same immutable offsets. OpenXR's located
geometry remains separate.

Successful matrix queries report their frame sequence, eye and clip planes. The
temporal pass uses the smallest near plane observed for that eye in the current
frame, retaining its paired far plane. It never invents depth planes or
substitutes the optional game-pose prediction for the render pose.

At Submit, input validation precedes GPU work. The producer calls the existing
temporal filter, then composites the menu. The host retains returned textures
until private eye capture completes. Separate-device transfer remains on its
existing ownership path. No GPU work or producer callback is added to shutdown
or WaitGetPoses.

Temporal inputs use the current frame and each eye's previous successful
treatment. Rotation and translation are composed through the complete previous
and current eye transforms. Jitter passed to the filter is expressed in actual
input pixels, including after a resize. Its sign/lag diagnostics affect only
the filter inputs, preserving their existing configuration meaning. DLSS
requests the recommended output size when both input dimensions are below the
existing upscaling threshold.

## Discontinuities and refusals

History resets after a frame gap, incomplete pair, invalid tracking,
reference-space change, resize, format/output-size change or temporal-mode
change. Stale contexts, stale sequences, duplicate eyes and wrong graphics
threads/devices are rejected. Closing the capability invalidates CPU admission
without releasing the graphics half's process-lifetime filter resources.

If filtering refuses an image, future jitter is disabled. The current raw image
retains its jittered FOV in both menu composition and OpenXR submission. A
jittered frame without a matching game matrix query is rejected because the
advertised projection alone does not prove what the game rendered.

Flipped UV bounds currently pass through and disable subsequent jitter for that
eye. The existing temporal shader does not account for flipped motion geometry.
Ordinary positive bounds and cropped/double-wide inputs retain the existing
region semantics. AA-off and flipped-input paths also reach the untreated
eye-dump entry point.

## Desktop validation

The provider fixture covers 157 checks using real WARP textures and a recorded
filter callback. It tests producer/CPU thread separation, stale state,
reference changes, gaps, resize, asymmetric jitter, changing eye rotation, head
translation, clip-plane selection, configuration latching, diagnostic sign/lag
semantics, refusals, and the actual native client table.

A separate test loads the actual graphics DLL and processes 18 eye images in
each of native TAA and DLSS modes. Both final runs passed 169 checks for owned
output, dimensions, color readback, shader-resource/unordered-access binding
restoration, resizing and CPU close. The DLSS run also confirms that NVIDIA
features were created for both eyes. These synthetic scenes do not qualify
Elite's captured scene depth or world-motion inputs.

The full build passed with all 490 source hashes unchanged, including the
native system/projection and existing shutdown, shared-transfer, bootstrap and
configuration gates. The paired DLLs are installed and hash-verified in
Frontier. The live INI and preserved original runtime DLL retained their
hashes. No launcher was started. In-game qualification remains pending.

## Next Frontier flight

Launch manually with the native runtime and SteamVR closed. Check cockpit text
and scene geometry while still and during slow head translation and rotation.
Use F8 to compare AA off, native TAA and DLSS, then close the menu and exit
normally. Insert remains available for eye dumps if an artifact is visible.

Review the matching build's native temporal engagement and summary records,
current-frame projection-query coverage, both-eye treatment, history resets,
depth/world-motion evidence and complete native shutdown. Keep detailed local
flight evidence separate from the public functional result.

This checkpoint does not complete terrain overscan/crop, supersample resolve,
sharpening, withholding, Explorer Cam, theater/heal or gaze migration. Existing
configuration remains intact; no full feature-parity claim is implied.
