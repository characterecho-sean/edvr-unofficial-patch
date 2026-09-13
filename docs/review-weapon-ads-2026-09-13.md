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
