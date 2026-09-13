# Weapon ADS alignment, 2026-09-13

The user reports a pistol's holographic sight pointing high and says the
sniper scope aligns correctly. Earlier feedback described opposite
vertical errors on different weapons. Do not apply a universal reticle
offset, change game aim, or assume all ADS geometry needs the same fix.

## Captured build and geometry

The first Steam flight is `edvr_gfx_20260913_055421.log`, verified
through `tools/edvr_log.py` as `v0.16.1-2-g857a842`, linked 11:49:44
UTC. The first check against the preceding `59216e8` correctly reported
a mismatch. Diffing those commits confirms that weapon stability and
weapon motion are unchanged; 857a842 adds other diagnostic captures.

Runs 060418 and 060419 show the pistol aiming down sights. The 060418
drawstate contains 19 source frames, 1,501 mesh draws and 57 frame-local
buffers, with no buffer failures or budget declines. Weapon stability is
On throughout this flight; there is no Off comparison in these dumps.
The captured flat source is 5120x2880. The holographic sight's raised
appearance is already present there, before curved-screen projection.

The first source draw is weapon geometry, so its original pool, palette
and instance stream were copied at that draw. Later references retain
that provenance rather than claiming to be fresh copies. The rigid
pistol pieces use records 13, 18, 36, 8, 29 and 21; the paired arm
records are 263..266. The first draw's arm-to-camera correction is
(0.041941643, 0.009459019, -0.062137604) metres, magnitude 75.562 mm.
The live status sample at 06:04:14.605 reports this same correction.

Across the 19 frames, source camera position changes by up to 0.452 m on
one world axis while this offset varies by less than one micrometre.
This is a persistent offset in this captured pose, rather than the
doubled/repeated camera-step discrepancy that motivated the original
judder fix. In the first camera basis the correction is about 0.576 mm
right, 9.473 mm up and 74.964 mm backward. The captured ADS near plane
is 0.025; the separately observed hip-fire projection uses 0.0675.

## Original-shader replay

The isolated replay executes the original vertex shaders for 21 captured
draws with the captured constants, palette, instance IDs and complete
indexed vertex windows. It compares stock and production-corrected
pools, with clean D3D debug validation and 779 harness checks passing.
The largest 24,819-index body draw exceeds its captured vertex window
and is explicitly excluded. This is not a complete material replay.

The holographic reticle is the six-index draw 82, original VS
025B4B9FF54622ED and PS 46F92DC71BF8DFA5, reading rigid record 2. The
surrounding glass is draw 83, VS 7F9B650EC1A1E570 and PS
F349CD33A0DAB8C7, using record 13 like the main body. Both already pass
the stability material gate. Their original bytecode was recovered from
the installed game; missing pixel shaders were extracted from
Effects2_Win64_SM50.arc and verified by EDVR's bytecode hash.

The reticle PS samples its alpha texture using interpolated UVs and
material constants. It does not independently reconstruct a camera
position. The reticle and glass therefore share the attachment
translation; adding another material hash would not address this case.

The native source crop shows the aiming marker above the front sight.
Mapping an approximate marker-tip position (2560, 1345) through the
reticle quad's fixed-to-stock homography gives (2554.016, 1457.197). The
source image centre is (2560, 1440). Mapping the corners agrees with the
original VS outputs within 0.000001 pixels. The marker coordinate is a
visual estimate, not a recovered alpha-texture feature; the snapshot
does not retain the reticle texture array. This supports an ADS
placement error from removing the persistent offset, but does not
measure bullet trajectory or establish the user's Off result.

Ruled out: curved-screen projection alone causes the raised marker,
because it is present in the original flat source. Ruled out: a missing
reticle/glass material family, because both affected original shaders
already use the same corrected pool. The original correction removes the
whole camera-to-arms offset; it has no distinction between timing error
and deliberate ADS camera placement.

## Live comparison and correction

The user confirms that Weapon stability breaks sight alignment. Steam
flight `edvr_gfx_20260913_061535.log` also verifies 857a842. Its toggle
history establishes that 062231 was Off and 062257 was On. Both captures
have 19 frames, 1,197 source mesh draws and 57 frame-local buffers, with
no failed copies or buffer budget declines. The flat source images show
the different weapon/reticle placement. They are separate views after
firing, so their pixels should not be subtracted as a stationary pair.

A correctly aligned sniper is useful coverage but is not evidence that
all weapons have the same intended ADS camera offset. Preserve that
working scope while establishing which pose component should be changed.

There is also a prior user report of scoped Tormentor crosshair/barrel
misalignment in the [Frontier forum discussion from May
2024](https://forums.frontier.co.uk/threads/tormentor-mods-one-must-go.624987/).
The captured weapon's engineering is not established. Ruled out: a stock
game sight bug alone explains this report, because the user's
same-weapon live comparison identifies the EDVR toggle as the trigger.

The positional correction now requires the verified separate hip-fire
projection, near 0.0675. ADS (0.025), projection transitions and other
unverified projections retain the complete original instance pool.
Preserving the original aiming pose also preserves differences between
weapons rather than substituting a universal translation or angle.

The GPU anchor is explicitly invalid on this path, so emitter and light
passes cannot retain a previous hip-fire offset and detach from the
stock mesh. The existing settings-upload invalidation refreshes the
anchor when entering/leaving ADS in the same frame. A separate log
reason identifies a projection that preserves game aiming pose, with its
near plane, rather than reporting it as a missing arm root.

The original-vertex weapon motion pass still runs for eligible opaque
ADS draws while temporal AA is enabled. This change preserves its
animation/projection motion and history-rejection behavior. The existing
Weapon stability toggle still controls both parts; no new setting or
runtime reprojection change is introduced. The original positional
movement judder may remain while aiming: this correction preserves
accurate sights and does not claim a verified timing-only ADS solution.

The expanded production-path WARP regression passes 1,364 checks. It
covers hip-to-ADS and return transitions, different projection scales,
all optic/material paths, fresh camera data, continued motion-pass
forwarding, and original emitter/light placement during ADS. The
captured replay checks all 57 ADS pools from 060418/062231/062257 remain
byte-identical to the game. Every original vertex output from the 21
complete pistol draw windows is bit-identical to Weapon stability Off.
The earlier 19-frame 173628 strafing fixture retains its independent
expected hip-fire correction exactly. D3D debug validation is clean.

The full absolute-path worktree build passes, including 80,493 existing
weapon-motion checks and the 252-key configuration contract. The built
DLL passes the NVIDIA smoke test, including DLSS motion conventions.

Local source images, original-shader exports and replay outputs are in
`build/review_motion/sep13/weapon_ads/`; game assets stay out of source
control.

## ADS movement after preserving aim

The user confirms e2b8939 fixes pistol ADS alignment, but strafing and
forward walking now judder during ADS. Steam flight
`edvr_gfx_20260913_063841.log` verifies `v0.16.1-3-ge2b8939`, linked
12:34:58 UTC. Captures 064206 and 064215 use the original source pose at
near 0.025, as the new diagnostic explicitly reports. The run uses
OpenVR, preset K, 2774x2740 input and 4268x4216 output; source colour is
5120x2880. Each capture retains 19 source frames and 57 buffers, with no
buffer declines. The first weapon draw owns each pool copy.

Confirmed: this is still camera/attachment update disagreement. In
064206 frame 20400, camera translation advances by (41.530, 58.662,
28.243) mm while the arms advance (16.891, 23.940, 11.501) mm. The
camera then repeats while the arms catch up. The stable camera-to-arms
offset is approximately (0.571, 9.439, -75.085) mm in camera axes; its
right component jumps to -45.174 mm on that frame. In 064215,
forward-camera offsets jump by 38 and 121 mm before returning to the
same aiming offset. The first capture contains another normal-play jump
after the initial capture stall; do not attribute the initial stalls
themselves to normal performance.

Ruled out: re-enabling the original ADS translation, because it would
remove the verified intentional aiming offset again. A separate earlier
aiming/crouching capture, 060857, has a substantially different offset
(about +130 mm forward); no fixed pistol calibration can cover both. The
Off/On controls 062231/062257 also contain small intentional pose
changes during mouse movement, so simply holding every observed offset
constant is not a valid correction.

The correction learns an offset after three consecutive synchronized ADS
samples. It corrects translation overshoots only while camera
orientation/projection remain unchanged and camera/attachment motion
agree in direction. A repeated arms origin requires preceding measured
locomotion. The error must equal the disagreement between their steps,
including any preceding correction; at most two consecutive overshoots
are corrected. Unrecognized changes, projection transitions, missing
history and discontinuities retain the game pose and reacquire
synchronization. This is deliberately conservative during changing aim.

Two eight-row GPU samples extend the anchor from 48 to 304 bytes. Each
draw reads only the preceding frame's sample, preventing multiple
materials or same-frame buffer rewrites from training the offset twice.
Effects can refresh their correction but cannot write timing samples.
Unknown command-list writes and live toggles invalidate a sample epoch.
The existing two compute dispatches and 48-byte asynchronous diagnostic
readback remain; the frame/epoch input is one 16-byte constant buffer.
No CPU geometry readback, weapon-specific calibration, image smoothing,
new settings, or compositor/reprojection changes are involved.

The expanded WARP test passes 1,962 checks, including forward/backward
and strafing overshoots, camera/arms repeats, sight/FOV and mouse
changes, scope reacquisition, same-frame rewrites, effect attachment,
source gaps, missing paired roots, live toggles, command-list
invalidation, and state restoration. The captured sequence replay passes
with clean D3D debug validation. All 76 frames in
060418/060857/062231/062257 remain byte-identical, including the earlier
mouse and crouching controls.

Starting without prior history, the replay corrects the normal-play
20400 jump by (24.638, 34.722, 16.742) mm after establishing the offset
from earlier synchronized frames. The forward capture begins on its bad
frames and cannot supply their missing prehistory. Separate explicit
warm-state tests prepend three synthetic synchronized samples using the
capture's measured offset and motion. Those correct both forward jumps,
and the initial strafing jump, against independently computed expected
pools. These prefix samples are not represented as captured frames. A
fresh ADS entry may therefore briefly retain stock judder until
synchronization is established; live headset confirmation is still
required.

The full absolute-path worktree build and NVIDIA smoke test pass. The
newer main-branch planet-motion changes are retained for the combined
test build; the weapon changes do not alter those paths.

## Turbo mode comparison

The user suggested an on-foot equivalent of OpenXR Toolkit Turbo mode.
The upstream [frame
implementation](https://github.com/mbucchia/OpenXR-Toolkit/blob/main/XR_APILAYER_MBUCCHIA_toolkit/layer.cpp)
starts the next `xrWaitFrame` asynchronously after submission, returns
an estimated display time without waiting when necessary, and
synchronizes again before `xrEndFrame`. It allows one frame of
pipelining and defers the real `xrBeginFrame`. Changing that scheduling
could avoid the observed update disagreement, but this is an inference,
not a verified Elite result from these dumps.

The upstream [menu
code](https://github.com/mbucchia/OpenXR-Toolkit/blob/main/XR_APILAYER_MBUCCHIA_toolkit/menu.cpp)
warns that Turbo can prevent motion reprojection, Oculus ASW, or SteamVR
motion smoothing. EDVR currently forwards OpenVR `WaitGetPoses` and
`Submit`; an equivalent needs a separate scheduling implementation and
runtime-specific validation. On-foot gating is possible, but it does not
remove that compatibility issue. The user chose to preserve reprojection
and finish the targeted ADS correction. No Turbo code is included in
this change.

## Residual judder on 7472e7c

The user confirms firing still aligns, but ADS strafing and forward
walking judder. The sanctioned log reader verifies Steam flight 071602
as v0.16.1-6-g7472e7c, linked 13:08:23 UTC. Runtime and image sizes
match the preceding run. Captures 071822 (strafing) and 071830 (forward)
each retain 19 frames and 57 buffers without declines.

The first strafing frame, 7589, has a 30.282 mm lateral offset error;
the camera repeats in 7590 while the arms catch up. The first forward
frame, 8252, differs by about 49.175 mm along travel and likewise
catches up on the following frame. Subsequent captured offsets are
stable to micrometre precision. Both jumps occur at the capture
boundary; neither capture contains the history that the live correction
used before it. These boundary stalls cannot establish the frequency of
ordinary judder.

Ruled out: 7472e7c completes the ADS fix, because the user still sees
judder. Ruled out: the existing tests fail only on NVIDIA or under the
live shader compiler flags, because all 1,962 checks pass on hardware
with both strict and production compilation. The complete captured mesh
draw order also retains synchronization through later scenery/material
draws in an isolated hardware replay. It still corrects the earlier
20400 overshoot. This does not prove the same history survives live.

Remaining discriminators are the live sample frame/epoch, acquisition
confidence, source gaps, aim/projection changes, and camera/arm steps
before a jump. Add bounded GPU prehistory to the eye-dump trigger rather
than changing sight calibration or relaxing the motion guard without
that evidence. The trace must preserve rendering and avoid routine CPU
readbacks, extra dispatches, or waiting for the GPU.

The eye-dump trigger now freezes the preceding 128 GPU frame samples,
including the first and last material's camera/arms positions, basis,
projection, learned offset, confidence, correction and guard flags. The
32 KiB ring lives after the existing anchor data and never feeds the
correction. Only the explicit eye-dump request creates/copies staging;
the frame-boundary drain uses DO_NOT_WAIT after three frames. The log
reports allocation/readback failure distinctly from completed capture.

The expanded production test passes 3,077 checks on WARP and NVIDIA with
the live compiler flags. It exercises ring wrap, first/last
preservation, exactly bounded readback, disabled/inactive requests, the
full logging path, and unchanged corrected/uncorrected geometry. This
build changes diagnostics, not sight alignment, motion correction or
reprojection. The next test should hold ADS and strafe continuously for
at least two seconds before pressing the eye-dump key while still
moving.

## Sustained one-frame delay captured before the dump

Steam flight 074127 verifies d0c5cf1, linked 13:38:04 UTC. The new
074439 trace retains all 128 pre-dump frames, 7561..7688, and both
material samples per frame. Camera and arms move throughout. The current
camera-to-arms lateral offset varies from -36.420 to -23.212 mm. In
contrast, the preceding camera minus the current arms is stable at
(0.571, 9.442, -75.087) mm in camera axes: its per-axis standard
deviation is below 0.001 mm over all 127 adjacent pairs. The subsequent
draw snapshot recovers that same original ADS offset after the capture
stall. This confirms a sustained one-frame translation delay, not just
isolated camera overshoots or a fixed aiming calibration error.

Ruled out: requiring three same-frame synchronized offsets can acquire
this movement, because it remains one frame out of phase throughout the
entire prehistory. Compare the arms' step with the preceding camera step
and learn the offset in that time domain. Require repeated agreement and
changing step sizes to distinguish real lag from the ambiguous case of
constant-speed synchronized motion. Preserve original geometry during
unrecognized aiming/projection changes and when returning to
synchronized updates. Do not remove the measured per-weapon ADS offset.

The trace's integer flag bitcasts read as zero even in the hardware
regression where status 2/3 and confidence 3 prove valid history. Store
the small flag mask as a numeric float and assert its decoded value; the
logged positions, status, frame stamps and confidence are unaffected.

The GPU now keeps two additional rows per frame bank: the lag-domain
offset/confidence and the measured camera step/evidence count. It
requires three varying-step correspondences before selecting that phase;
a single camera overshoot during constant-speed synchronized movement
must still use the earlier isolated correction. Once recognized, it
translates the attachments by the current camera step and retains the
lag-domain ADS offset, including each weapon's distinct forward offset.
It handles steady velocity and reversals after acquisition, then returns
to untouched geometry when the game's updates synchronize or the view
changes. No extra dispatches or routine CPU readbacks are added.

The recorded timing replay applies all 124 expected corrections after
four acquisition frames. This replay transplants the 128 logged camera
and arms samples into the complete first captured instance/palette pool;
it does not claim those animated geometry buffers were captured before
the eye-dump key. Expected translations are independently computed from
adjacent logged camera positions. Full-pool comparisons pass alongside
the original mouse, crouch, sight and isolated-overshoot controls (4,130
checks). Original snapshot frame 7702 also has an isolated 34.043 mm
overshoot; the prior correction handles it and the next frame catches
up.

The hardware timing replay also passes all 4,130 checks with the same
124 sustained corrections. The full absolute-path worktree build,
expanded weapon tests, 252-key configuration contract and NVIDIA DLL
smoke test pass. Live headset confirmation remains necessary, especially
when entering ADS or changing aim before the phase can be established.
