# Native startup centering

The user confirmed working sharpening, then reported that the splash screen
started directly behind and significantly above them while wearing the headset
and facing forward. This flight used SteamVR's OpenXR runtime. The matching
`v0.16.2-88-gbe44d38-dirty` trace starts at 12:46:29 UTC; its first seated
reset is at 12:47:00, about 31 seconds later. There is no startup recenter in
the native host. The live legacy `fix.launch_centre` setting is `auto`, but
that OpenVR hook does not run in the native module.

The splash is anchored in the game's forward direction. The intro panel
implementation deliberately shares that anchor, so an uncentered tracking
origin can displace both. This is the source-backed explanation for the
reported placement; the old trace did not record the initial head transform.
The correction therefore records the sampled transform and post-reset error to
distinguish successful origin correction from any remaining panel problem in
one flight.

## Agreed behavior

The user explicitly chose always-centered native startup with no toggle. This
supersedes the original port plan's proposal to retain the legacy toggle for
native OpenXR. The native path ignores `fix.launch_centre` and the legacy
angle/distance guards. The existing OpenVR settings and behavior remain intact.
There is no vendor API or runtime-name selection: Windows' selected OpenXR
runtime remains in use.

At startup, the owner finishes a zero-layer frame and samples the current head
pose in the natural LOCAL space using the converted performance-counter time.
It requires valid and tracked position and orientation, finite pose values and
a usable horizontal forward direction. Known identity-at-zero startup
placeholders do not choose the origin. The new seated space uses the sampled
translation and heading only, leaving the world upright and retaining the
head's pitch and roll relative to it.

Reference-space replacement occurs under the runtime operation lease with no
open frame. Startup invalidates cached geometry and temporal history, advances
the origin generation and then obtains fresh geometry before returning any
interfaces to Elite. Display dimensions remain available across invalidation.
No synthetic application reset event is queued before initialization completes.
Explicit game recentering retains its event and uses the same origin helper.

The startup policy cannot rearm during play. Unusable tracking is allowed up to
two seconds after the first usable startup geometry, bounded additionally by
600 samples. If tracking remains unusable, it logs that centering could not be
established and leaves the runtime origin unchanged, rather than resetting
later during play. A failed space replacement stops initialization because the
space can no longer be used safely. Normally a tracked pose centers immediately
and requires only one additional geometry frame.

## Qualification

Desktop policy and native-host regression checks cover the reported backward
heading and vertical offset, upright geometry, tracking placeholders, missing
valid/tracked flags, invalid poses, one-shot operation, bounded waiting,
reference-space failure, open-frame exclusion and explicit reset events. The
final full build passed with all 514 source hashes unchanged, including 24
policy checks and 60 native host checks (19 new host regressions), the five
transport cases and all existing startup, timing, stereo and shutdown gates.

The first full build exposed a race in the existing Present fixture:
`waitForRender` wakes the Init worker inside the first Present callback, before
its queue pump. The fixture could queue and complete its command in that
callback, then incorrectly require it to remain pending for the next Present. A
fixture-only event now waits for the first Present to return before queuing the
command. All five isolated WARP transport cases pass after that correction;
production transport behavior is unchanged.

The paired `v0.16.2-89-g419cbe5-dirty` DLLs are installed and hash-verified in
Frontier, with the game INI and original OpenVR DLL preserved. Startup still
uses `runtime=system`. Exact source hashes, binaries, full build output and
installer verification are archived locally under
`build/openxr-launch-centre-20260914/`. The headset check remains pending.

The next manual gate uses Frontier with the headset already worn and facing
forward: the splash and intro should start in front at seated eye height,
remain anchored during head movement, and transition normally into the menu.
Manual recentering and normal exit should still work. Inspect
`native_launch_centre,applying=1`, `applied=1`, post-reset position/yaw errors,
the following `runtime_startup` geometry sequence and normal shutdown. An
explicit `tracking_deadline=1` is a tracking-readiness failure, not a centering
pass. Experimental features and performance comparisons remain deferred.
