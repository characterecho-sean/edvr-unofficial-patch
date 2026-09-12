# Rectangular haze gaps: 12:19 capture and AA-dependent depth writes

Reviewed 2026-09-10 on `codex/smoke-temporal-history`. This follows the
[12:02 review](review-smoke-capture-1202.md). The source changes remain
uncommitted; the installed DLL is build `6AA2F31A`, linked at 18:12:42 UTC.

**Follow-up:** the user reports that the holes disappeared with UI depth
disabled. Private-depth build `6AA2FDEB` is now installed with UI depth
enabled again. The corrective implementation and its validation are recorded
at the end of this review; the diagnostic sections below describe the
preceding build.

## What the new capture establishes

The inputs are `edvr_gfx_20260910_121645.log`, the `eye_121951` images and
their draw ledger. The user confirmed the specific artifact: sharp
rectangular gaps in the faint blue column above the radar, visible around
x=1010, y=710–880 in the 1,400-square raw `C00` crop. The user also reports
that the gaps appear only with TAA or DLSS enabled.

The gaps already exist before the final temporal resolve. That identifies
an earlier stage to investigate; it **does not** identify a stock-game
defect. AA has already changed projection jitter and enabled auxiliary
depth processing by the time the raw crop is captured. Both TAA and DLSS
share those changes.

The previous diagnostic was active. Its periodic log names the exact
`9F4BBCFCD3B68BC9`/`9AEC596A2B036EA6` skip and reports 208,422 skipped draws
by 12:19:46. These families still appear in the census and ledger because
those instruments record the draw **before** the diagnostic withholds it.
Their presence in the census is not evidence of a failed switch.

The user thought there might be less smoke, but the confirmed rectangular
gaps survived. Suppressing these two families is therefore not a solution
for the remaining blue haze. The known ribbon family `5E417E9DF2E7F9E6`
was also withheld by `drives_smoke = off`. The generic plume replacement
family `EB787F983BC1F5A3` has no recorded draws in the first census frame.
The restored planetary family `203DF51758AADC4D` appears as expected.

The fresh shader dump supplied the previously missing
`9AEC596A2B036EA6` VS and `3789CA2062E196FB` PS. The latter samples a
particle texture array, applies particle alpha/color, and fades against
sampled scene depth. It outputs color without `SV_Depth`. This confirms
the kind of rendering it performs, but the negative skip result prevents
assigning the surviving gap to it.

## Why UI depth is the next controlled test

`uiDepthConfigure` in [ui_depth.cpp](../src/d3d11/ui_depth.cpp) gates the
whole feature on both `fix.ui_depth` and a non-off `fix.temporal_aa`.
Native TAA and NVIDIA modes therefore enable the same depth-writing work.
`advanced.ui_depth_reactive = 0` disables the reactive mask; it does not
disable these depth writes.

At scene-projection HUD/hologram draws, `uiDepthReissueBegin` binds the
game's depth target and repeats the geometry with a coverage pixel shader.
Its depth state enables writes and a nearer-wins test. The dedicated
private-depth branch currently handles only the known smoke ribbon
(`g_reissueMaskSlot == 3`). Other coverage passes still modify the scene
depth that later game draws can test against. Restoring the bindings after
a draw does not undo the depth values written into the texture.

The 12:19 run logs coverage passes for the vector HUD, holo material and
sprite families. The first-eye HDR census contains vector-HUD draws before
other transparent draws. This makes unwanted occlusion of later effects a
plausible explanation for holes already visible in raw color. It does not
prove which draw wrote an offending rectangle or that depth writes are
the cause; the census records state and order, not intermediate pixels.

Projection jitter remains a separate candidate. Its live control is
`experimental.temporal_aa_jitter`, as read by
[temporal_aa.cpp](../src/openvr/temporal_aa.cpp). It stays enabled for the
depth test so these two causes are not switched off together.

## Installed diagnostic, awaiting a live result

The game was closed. Only these three installed INI keys changed:

| Key | Before | After |
|---|---|---|
| `fix.ui_depth` | `on` | `off` |
| `advanced.census_skip` | the two billboard VS hashes | empty |
| `advanced.glare_shader_dump` | `1` | `0` |

DLSS, projection jitter and per-object motion remain enabled. The existing
smoke and heat-haze switches retain their settings. Clearing the diagnostic
restores the two particle families; shader dumping has supplied the missing
bytecode and is no longer needed. Because the skip is also being retired,
a same-scene comparison toggling only `ui_depth` is the decisive A/B.

The prepared and installed INI hashes match. The preparation checks that
exactly these three active settings change and backs up the previous INI.
The installer refuses to overwrite settings changed since preparation and
does not replace DLLs. Artifacts are under
`build/review_motion/smoke1219/depth-diagnostic`; the installed INI SHA-256 is
`B6A58241F9C42DA976B1C6E854FB73D33AA5C0862CCC00A568C9DE3C2BE551A9`.

The next eye dump should keep the blue column in view with AA on. If the
gaps disappear with UI depth off and return when it is enabled in the same
scene, isolate the responsible family and move its temporal-only depth
contribution out of the game's live depth. That is a candidate code fix,
not a result established by this capture. Disabling UI depth can reduce HUD
temporal stability, so this setting is a diagnostic rather than a shipping
default. If the gaps remain, the next independent comparison is projection
jitter, with AA still enabled.

## UI-depth-off result and corrective implementation

The user reports: "I don't see the holes anymore." The subsequent
`edvr_gfx_20260910_123703.log` confirms DLSS engaged at 2,862 by 2,826, with
the broad particle skip cleared and no UI coverage engagement. This is
strong evidence against a defect confined to the final AA resolve and
for the added coverage-depth path. The exact HUD family responsible for
each visible rectangle has not been isolated by an individual-draw A/B.

The implementation now keeps all supported HUD and interface coverage out
of the game's live depth. `UiDepthLayer` owns a private texture per eye,
seeded from the original scene depth at the first coverage draw each
frame. This preserves the coverage pass's occlusion by scene geometry.
Further UI coverage accumulates in that private texture. The earlier
private smoke layer remains separate.

The temporal depth accessor merges the current scene, smoke and UI layers
using the nearer depth in reversed-Z. Native TAA and both NVIDIA dispatch
paths bind the UI layer at `t7`; a late nearer scene draw still wins over
the private snapshot. The UI layer is published only for the matching
source identity, dimensions, eye and current frame. An unsupported target
or missing coverage shader declines the added pass; there is no fallback
that changes the original draw's depth state.

The reissue also restores the original PS constant buffer at `b13`, and
the temporal pass saves/restores all eight SRV and six UAV slots it touches.
Per-object motion selection and its station transforms are unchanged.

`tools/ui_depth_test/ui_depth_test.cpp` includes the production coverage
implementation and runs it on D3D11 WARP. The synthetic positive control
reproduces a rectangular smoke hole by writing coverage into live scene
depth. The corrected pass leaves the smoke intact, while AA receives the
covered depth. It also tests alpha clipping, scene occlusion, restored
bindings, menu projection/rebinding, disabled and empty frames, eye/source
identity, resizing, three depth encodings and MSAA refusal. A GPU test
compiles the actual temporal depth accessor and verifies the UI merge,
late nearer scene depth and an absent UI layer. All 863 checks pass.
The system D3D debug layer is optional; WARP tests run without it when it
is not installed. The harness is part of `build.bat`.

This adds one depth copy per participating eye per frame and one extra
depth sample in the temporal accessor. Memory is one additional scene-depth
texture per eye (about 123 MiB total at 2,862 by 2,826 for the 64-bit depth
format). Live performance and HUD/station appearance still require a
headset comparison with UI depth restored. The source fix's synthetic
result is not a claim that this headset validation has already happened.

## Installed corrective build

Build `6AA2FDEB` linked at 18:58:51 UTC (12:58:51 local). The full NVIDIA
SDK build passed, including all 3,895 switch/capture checks, the 863 new
coverage/merge checks and the existing build-time tests. The proxy GPU
smoke suite also passed. Both DLLs were installed while the game was
closed and verified against their prepared SHA-256 hashes.

The only installed INI change for this deployment is `fix.ui_depth` from
`off` back to `on`. DLSS, jitter and per-object motion remain enabled;
the previous broad particle skip remains cleared. All other settings were
preserved. Backups, candidate INI and the deployment manifest are under
`build/review_motion/smoke1219/private-depth-install`.

The user subsequently confirmed that this installed build fixed the smoke
holes with UI depth restored: "Great, that fixed it." The remaining report
is whole-station flicker during movement, investigated separately in
[the 13:02 flight review](review-station-flicker-2026-09-10.md).
