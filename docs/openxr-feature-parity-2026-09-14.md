# Native OpenXR feature parity and consolidated retest

Sean requested continued work while away from the computer, with one retest
list afterward. This checkpoint covers the remaining supported submission
features. Performance comparisons, FSS theater, supersample resolve and gaze
foveation remain deferred. Windows selects the runtime; no vendor runtime is
hard-coded. No game launch or headset result is implied by desktop validation.

## Implementation

The graphics provider receives the physical headset pose before Elite renders.
This restores the shared physical pose and head-forward channels used by the
intro panel and head-responsive graphics fixes. Explorer Cam reads the existing
offset, yaw and external-camera gate settings once per frame. Its adjusted
`WaitGetPoses` render pose (and game pose when configured) is returned to Elite
and supplied to temporal reprojection. Separate System pose queries, the OpenXR
layer and menu retain physical tracking, matching the legacy offset
interception point. Gate/config changes retire history and saved transition
images at the next frame boundary.

The terrain cull guard preserves its existing headset allowlist and scene gate.
It first requests larger game render targets while reporting the true
projection. Only a changed, sufficiently enlarged submission from both eyes
allows the next frame to use the expanded frustum. The true-field-of-view crop
follows temporal AA and precedes sharpening, the menu and native composition.
Signed input bounds are composed with the crop, preserving flipped images.
Runtime swapchain dimensions remain the runtime's recommendation. Recenter
retains target-adoption evidence when the optical projection and dimensions are
unchanged. A guard waiting for target adoption cannot safely be forced active.

Transition suppression takes one decision at the first valid eye submission.
Both eyes then reuse the last successfully completed stereo pair, including its
physical poses, field of view, bounds and color interpretation. The two capture
buffers exchange ownership after successful frame completion, avoiding an extra
full-image copy. The game cannot overwrite the saved pair. With no compatible
pair, or with resubmission disabled, the host submits zero layers and continues
normal OpenXR frame pacing. Recenter and scene-clear operations retire replay
state. An omitted transition frame never enters AA history: floating-origin
changes retain history when the existing detector says the camera stayed;
returned jumps, explicit holds, healed images and unanswered verdicts reset it.

The FSS arrival repair uses the existing graphics shader and input-lockstep
settings. Its donor textures are owned copies and never mutable game pointers.
The normal repair is restricted to the arrival/chrome window and the measured
display rectangle. The existing developer mirror mode retains its bounded-tile
detector. The legacy shader's projection assumptions must hold; unsupported
canted/asymmetric geometry passes through and is reported, rather than applying
an incorrect disparity correction. FSS theater is outside this checkpoint.

Native startup centering is unconditional, as requested. The preceding
[centering checkpoint](openxr-launch-centre-2026-09-14.md) remains part of this
combined retest because its headset check has not yet occurred.

## Desktop qualification and installed pair

The absolute-path full `build.bat` run passed with all 526 source hashes
unchanged. This includes 35 frame-provider checks, 74 FSS ownership/gating
checks, 64 checks using the actual FSS HLSL on WARP, 32 cull-policy checks, and
213 temporal-provider checks. Capture and shared-device transfer passed 142 and
267 checks; frame pairing passed 154. The native host, startup-center,
shutdown, module integration, transport matrix, Python self-tests and all 254
config contracts also passed. Separate real-DLL GPU runs passed 169 checks and
18 treated eye images each for TAA and DLSS, plus 58 sharpening-on checks and
22 sharpening-off checks, including output readback and provider counters.

Parent review corrected hold consumption during non-rendered frames, stale
replay after configuration changes, FSS donor validity/geometry/stamp handling,
shader resources retained across device lifetimes, and cull adoption after
recenter. The supported gameplay review has no outstanding code blocker; visual
behavior still requires the retest below.

Frontier is installed and hash-verified with build `v0.16.2-90-gda62976-dirty`.
The qualified source snapshot and DLLs are archived locally under
`build/openxr-feature-parity-20260914`, with the complete build log and
qualification record. Native DLL SHA-256:
`D2B0CD5204956CCC327FCED47587372E2AB42354619B56EF972AC8A6C1F89C9C`. Graphics
DLL SHA-256:
`506B6BD503A87976D069A410720DC0DD593B73EDA48D5F4162F6A4F4B828D1DF`. The live
`edvr.ini` and preserved original OpenVR DLL retain their prior hashes. Startup
configuration remains `runtime=system`. No game or VR runtime was launched for
this batch, and no headset result is recorded yet.

## Retest when home

Use the Frontier launcher normally. Begin with Pimax and the Windows-selected
SteamVR/OpenXR runtime, where the incorrect startup origin was observed. Repeat
the basic launch, menu, tracking and exit checks with Quest 3/Virtual Desktop
using Windows' selected VDXR runtime. Record which runtime/headset each run
uses. There is no need to compare frame times or match resolutions in this
pass.

1. **Startup and basic rendering.** Wear the headset facing forward before
   launch. The splash should appear ahead, level and at a sensible height. The
   intro should start promptly. Check both eyes, head movement, the main menu,
   F8 and the floating `gpu`/`cpu` values. Move and manually recenter; the menu
   and scene should remain aligned. Exit normally and relaunch once.
2. **Cockpit, on foot and Explorer Cam.** With the usual AA/DLSS and sharpening
   settings, check moving scene geometry, cockpit text and on-foot weapons.
   Enter and leave the external camera on foot, then return to the cockpit. The
   configured offset/yaw should apply in its configured modes, with stable
   stereo tracking and no persistent displacement after leaving the camera.
   Test the camera toggle several times and recenter once while it is active.
3. **Transitions.** Enter/leave FSS, use external-camera transitions, and make
   a normal supercruise or hyperspace transition when convenient. Watch for
   white/black flashes, mismatched eyes, lasting frozen images, or renewed DLSS
   flicker afterward. A brief reused frame is expected during a detected bad
   frame; normal motion must resume. Exit normally after these transitions.
4. **Terrain guard.** With the existing `fix.cull_guard` settings enabled and
   the headset allowed, inspect terrain near the edge of the view while moving
   the head. Also inspect text and the menu for stretching, wrong cropping or
   lost sharpness. If `native_cull` reports `stage=2` (awaiting larger
   targets), change HMD image quality in Elite and apply it so the game can
   recreate its targets; record the resulting dimensions and guard stage before
   judging the effect. `stage=3` means active; `stage=4` means the
   configuration or geometry was guarded off. Recenter once with the guard
   active; it should stay active.
5. **FSS arrival.** Open FSS several times and compare the two eyes during the
   arrival animation. The display should not have a one-eye black area or
   duplicated neon frame lines. Check normal FSS interaction, then close it and
   inspect cockpit text under head movement. The log must distinguish a tested
   heal from a projection guard that deliberately passed through.

For any issue, note the run, feature, AA mode and whether it affects one or
both eyes. Insert eye dumps remain useful for image defects. The build and
native feature summaries in `edvr_logs` identify the installed pair and which
paths actually ran; desktop counters alone do not qualify visual behavior.

## Remaining qualification

This batch requires the combined headset retest above. Developer-only cull
separability channels (`advanced.cull_guard_channel=raw/matrix`) and
alternative copy probes remain legacy diagnostics; native gameplay uses the
normal both- projection guard and bounds crop. Their diagnostic behavior is not
qualified by this batch. Broad performance/image quality comparison remains
deferred. Forcing Elite's legacy Oculus probe into OpenXR is a separate
startup-selection problem, tracked in the [LibOVR
investigation](openxr-oculus-selection-2026-09-14.md). Static analysis now
establishes the loader call and fallback loop; early native-package activation
and a controlled launch still need qualification before suppression is enabled.
