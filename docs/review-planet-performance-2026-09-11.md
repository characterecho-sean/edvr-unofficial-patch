# Planet-side AA cost and walking weapon continuity

## Evidence and scope

The user reports much better weapon stability with 7c65d10, slight
remaining judder when walking forward/sideways, and about 16 ms after
entering the cockpit, falling roughly 10 ms with AA off.

Steam gfx log `edvr_gfx_20260911_184844.log` and its VR counterpart
match 7c65d10. Eye runs 185218 and 185242 show the walking rifle and
landed cockpit respectively, at 38 Lyncis 4 B. The runtime is Valve
SteamVR at 90 Hz; DLSS preset K uses 2268x2240 input and 4536x4480
output per eye. The on-foot source is 5120x2880. The offline benchmark
runs on the local RTX 5090.

Hypotheses and discriminators:

- Excess NVIDIA reconstruction: compare its timestamp bracket with the
  whole temporal bracket and the reported AA difference. The session
  averages are about 1.88 ms/eye for NVIDIA and 2.15 ms/eye for temporal
  work. The cockpit's reported door GPU is 4.46 ms stereo, leaving
  substantial AA work outside that bracket.
- Expensive terrain history: the cockpit capture contains 81/81 matched
  patches, and each patch launches a serial search through previous
  192-byte keys before reissuing its coverage draw. Benchmark those
  captured identities and compare complete output records.
- Capture overhead: explicit dumps allocate/copy hundreds of buffers and
  do cause hitches. Exclude their CPU frame spikes from normal-play
  cost. The reported sustained slowdown is not established by a capture
  hitch.
- Attachment correction dropping out: replay the production root
  predicate on every walking frame, including the actual bone matrices.
  This distinguishes a missing correction from residual animation or
  compositor pacing.

## Terrain history search

Confirmed: the one-thread terrain search is expensive even at the 81
patches visible here. Each invocation serially compares every full key.
The original loop costs a median 2.634400 ms per eye for these 81
dispatches; distributing the same comparisons across 64 lanes costs
0.341472 ms. The stereo saving is 4.585856 ms in this isolated replay.

All 81 complete 272-byte output records are byte-identical, including
keys, quaternion, position, validity and previous-frame transform. The
candidate retains full key comparison and counts all matches, including
duplicates in different lanes and in different strides of the same lane.
Only a unique match is accepted. The 512-record cap, coverage geometry,
depth checks and temporal consumer are unchanged.

The replay reconstructs current patch constants from captured records,
uses a valid synthetic source projection and a small translation, and
executes the production search against all 81 captured predecessor keys.
It measures the searches and constant-buffer bindings, not terrain
rasterization, DLSS, or total game frametime. Four warmup rounds precede
20 alternating timestamp samples per variant; disjoint samples fail the
test. Baseline range: 2.288736--3.520288 ms; parallel range:
0.328704--0.362016 ms. These results explain a substantial part of the
reported AA difference without claiming a measured in-flight saving.

Artifacts are under `build/review_motion/sep11/planet1850/`:
`terrain-old.hlsl`, `terrain-parallel.hlsl`, `terrain_bench.cpp` and
`bench.bat`. The installed capture supplies the original predecessor
records. Blocking readbacks and query waits belong only to the
benchmark.

Added a bounded terrain timing ring, sampling every 64th eligible draw
from preparation through coverage reissue and restoration. It polls with
DONOTFLUSH after at least four frames, never waits, and prints sample,
skip and invalid counts periodically and with an eye dump. Its cost is
explicitly separate from the existing EDVR-at-door GPU value. This will
measure the complete corrected path in the next flight.

## Walking weapon detector

Confirmed: the walking arm root rotates by up to 0.126 degrees. Its
basis remains rigid (maximum orthonormality error below 6e-7), and both
arm records retain the same bind transform, attachment origin and
orientation. The identity-only test rejects it in 17 of 19 frames; it
starts accepting again in the last two as the animated root crosses that
test's boundary. The required attachment correction reaches 57.75 mm.

Ruled out: a purely residual animation/pacing explanation for this dump,
because the attachment correction itself is absent in 17 captured
frames. This does not rule out all runtime pacing effects in other runs.

Use the structural invariant instead: a finite, orthonormal,
positive-determinant basis with the existing eye-height bind
translation. The partner must share the complete bind transform as well
as the attachment origin/orientation and a distinct palette base. Do not
change the correction magnitude, bone animation, reprojection or
temporal AA. Scaled, reflected or mismatched roots decline.

The detector now accepts all 19 walking frames, and retains all 95
accepted frames across the five earlier pistol/rifle/tool captures. GPU
replay of all 19 walking frames matches independently reconstructed
pools exactly; nontranslation fields remain byte-identical. The existing
live Weapon stability toggle controls this refinement too.

The production weapon regression passes 189 checks. Terrain regression
passes 258 checks, including unique matches at slots 0/63/64/511,
duplicate matches across lanes and within one lane, missing history,
independent eyes, coverage occlusion and state restoration. A separate
offline fixture triggers the sampled timing path and verifies completion
through Begin/End and deferred frame polling (259 checks).

Full SDK build and all repository gates pass, including the unchanged
244-key configuration contract. NVIDIA smoke passes TAA, DLAA, DLSS,
foveation and motion/jitter conventions. In-game frametime savings and
perceived walking smoothness still need a test flight with this build.

## Complete terrain path in the 20:02 cockpit capture

The next Steam flight, `edvr_gfx_20260911_195950.log`, matches 483471c.
The user confirms lower GPU cost, but a smaller gain than the isolated
search benchmark. Capture 200203 contains 81 matched patches per eye at
the same 2268x2240 input and 4536x4480 DLSS output, SteamVR at 90 Hz,
preset K. Capture 200256 is on foot.

Ruled out: treating the search-only benchmark as the complete in-flight
terrain saving. The new production interval measures 16.650 to 16.871 us
per patch, including its search, geometry reissue and restoration. At 81
patches per eye this is roughly 2.7 ms stereo. Its 3,825 completed
samples have zero skipped or invalid queries. The temporal bracket
remains about 2.04 ms/eye, of which NVIDIA reconstruction is about 1.79
ms/eye. Capture hitches and HMD-idle WaitGetPoses intervals do not
measure steady gameplay GPU cost.

Confirmed: every terrain depth draw in this cockpit census has VS
ACE405F428C17EF6 and a null pixel shader. The 81 draws total 6,919,680
indices, or 2,306,560 triangles per eye. Reissuing them duplicates
roughly 4.6 million triangles per stereo frame. The original VS samples
four patch textures and performs the terrain transforms again during
this reissue.

Record private patch index and raster depth in two colour targets during
the original null-PS depth draw. Keep the game's DSV, depth/stencil
state, sample mask, geometry and viewport. This preserves the original
depth/stencil effects while producing the existing temporal inputs. It
replaces no game pixel shader and does not export or change SV_Depth.
Alpha-to-coverage and non-null pixel shaders retain the existing reissue
fallback; a mixed coverage mode in one eye/frame declines subsequent
unsupported records. Coverage depth storage is allocated lazily for the
path used, so the normal single-draw path does not allocate the fallback
DSV.

The full-path hardware replay uses all 81 captured constant-buffer sets
and draw counts, the original installed vertex shader, a generated UV
grid and substitute terrain samples. It includes original terrain
rendering, production history search, private coverage,
constant-buffer/state changes and restoration. Both paths produce
byte-identical complete motion records and index images, plus identical
coverage and scene depths over 4,017,017 covered pixels. The samples are
synthetic: this is not a reconstruction of the exact terrain textures or
a measurement of the next flight.

After four warmup rounds, 12 alternating samples per variant on the RTX
5090 measure 1.385600 ms/eye with reissues (range 1.348512--1.636608)
and 1.098432 ms/eye with capture in the original draw
(1.061216--1.261760). The median saving is 0.574336 ms stereo,
approximately 21% of this complete terrain bracket. Do not extrapolate
the earlier search-only gain into another multi-millisecond saving.

The WARP production regression passes 403 checks. Added comparisons
preserve actual D32S8 depth/stencil and original colour with passing,
depth-rejected, stencil-rejected and sample-masked fragments. Raster
depth bias and non-default viewport depth match the original depth
exactly. Checks also cover published coverage, state restoration,
mixed-mode refusal, the non-null-PS fallback and AA off. The Windows D3D
debug-layer component is unavailable in this environment
(DXGI_ERROR_SDK_COMPONENT_MISSING); the tests run without it, so this
pass does not claim debug-layer validation.

Artifacts are under `build/review_motion/sep11/planet2000/`:
`terrain-input.bin`, `fused_bench.cpp`, `bench.log`, the extracted
original VS and the regression log. Production timing now labels
original-draw capture separately from reissues. Its bracket includes the
game's original terrain draw when capture shares that draw, so its
average cannot be directly subtracted from the preceding build's
reissue-only bracket. The next flight must verify activation counts and
perceived/total frametime.

Final full SDK build passes all repository gates, including the 244-key
configuration contract, 403 terrain checks, 189 weapon checks and the
C++/Python effect-capture round trip. NVIDIA smoke passes TAA, DLAA,
DLSS, foveation and motion/jitter conventions. The original-shader
replay also confirms all 81 patch histories are valid, not merely equal
fallback records. Settings and DLSS quality are unchanged.

## Flight 20:48, user sees no material DLSS performance change

Ruled out: the last optimization delivering a noticeable total frametime
improvement in this flight. The user reports no material change,
including after reducing terrain quality. Log
edvr_gfx_20260911_204813.log matches 39b2eac and confirms 1,050,619
terrain patches captured in their original draws with zero reissues; the
optimization was active. The terrain bracket averages 15.494 us/patch
including the original game draw. NVIDIA reconstruction remains
approximately 1.8 ms/eye. The previous replay saving cannot substitute
for this in-game feedback.

The steady cockpit intervals are more useful than the session average:
between 20:51:14 and 20:51:54, the cumulative submission counters imply
about 2.01 ms/eye, consistent with the roughly 4 ms stereo door GPU
measurements. The early 201.65 ms maximum includes startup; dump
readback also produces CPU spikes. Neither is normal reconstruction
cost. Sampled draw-hook CPU is roughly 0.5 ms in the cockpit. These logs
do not establish another large, safe performance saving. No further
terrain or DLSS quality change is included in this follow-up.

### Purple weapon emitter and escape profile, 20:51/20:52 captures

Environment remains SteamVR, RTX 5090, DLSS 310.7 preset K, 2268x2240 to
4536x4480 per eye; the on-foot source is 5120x2880. The two on-foot v5
snapshots (`205232`, `205235`) contain all 38 requested effect draws
with their original layouts and vertex/index streams, without failed
copies or budget declines.

The purple glow is the local particle draw using VS `9AEC596A2B036EA6`
and PS `3789CA2062E196FB`. It does not read the corrected instance pool.
Its CB0 rows 9..11 carry the emitter's camera-relative model transform;
the original vertex shader uses these to position every particle before
projection through CB1 rows 270..273. The emitter stays rigidly attached
to rifle record 40 before the mesh correction: its local offset is about
(0.00063, 0.00089, 0.07490) metres, varying by under 6 micrometres
across each capture. After moving only the mesh, relative vertical
displacement varies by 23.3 mm in the first capture and 14.6 mm in the
second; the overall attachment correction reaches 53.8 mm. This confirms
the missing emitter correction, rather than an independent animation or
DLSS defect.

The existing Weapon stability toggle now also corrects this emitter on
the GPU. The original vertex/pixel shaders, particle animation, colour,
atlas, geometry and blend state remain in use. Only CB0 translation is
substituted for the draw. The path requires a fresh source mesh anchor,
the verified shader pair, the captured 208-byte model layout, a proper
rigid emitter within the existing one-metre attachment volume, and the
first-person projection's 0.0675 m near plane. World particles use the
0.025 m projection. Other projections remain unchanged; this is a
deliberately bounded fix for the verified viewmodel path, not a general
particle attachment classifier. Resource writes refresh the anchor;
unknown command-list state invalidates it. No synchronous readback is
added.

Ruled out: the account display's glass appearance is the loading wash.
The `205155` menu composite samples the replacement 1x1 black wash at
t0, which collapses that blur term, and its 1781x1001 source contains
sharp profile text. All 1,571 bright pixels in the sampled profile text
box have UI coverage, but 112 still carry underlying cockpit hologram
11's depth. The original menu draw disables depth testing while the
private AA reissue unconditionally used a nearer-wins test. That
mismatch lets visible menu pixels inherit a nearer cockpit surface and
its motion.

Interface-projection draws with depth testing disabled now replace
private depth under their alpha coverage, matching the visible overlay
order. Depth-tested interfaces and scene geometry retain the existing
test. The game depth is still untouched. This addresses the demonstrated
overlap error; it does not promise to recover detail absent from the
menu source or establish that every perceived softness is resolved.

Targeted validation: 523 weapon tests and 20,718 UI tests pass,
including world/far/nonrigid emitter rejection, live toggling,
fresh-input rules, GPU binding restoration, and opaque/transparent
overlay ordering over nearer scene depth. All 38 captured particle draws
replay through Elite's original vertex shader; every output component
matches the independent attachment correction with zero measured error.
The original mesh-only path is the negative control. Evidence and replay
tools are under `build/review_motion/sep11/flight2100/`.

The full production SDK build and all repository gates pass, followed by
the actual NVIDIA DLL smoke checks for TAA/DLAA/DLSS and motion/jitter.
The WARP regressions ran without the optional D3D debug layer because it
is unavailable on this machine; the functional GPU/state assertions
passed. Neither fix changes settings or claims a further DLSS speedup.

## Flight 04:10, remaining emitter and cockpit UI

Ruled out: the first emitter correction resolves every visible pink
element. The user still sees a slight pink sprite rise during crouching
on build 22a3742. The same flight confirms the account-display
correction, but reports cockpit UI swimming with vertical head motion.
Capture 041300 is on foot; 041404 and 041409 show the cockpit. Preserve
the confirmed menu fix while tracing the regression. Performance remains
an open request; the prior isolated benchmarks are not evidence of a
user-visible gain.

Confirmed cockpit material omission: log 04:13:47.579 rejects VS
81216C77F90DEDD6 / PS B4786E0A0B199285 as unsupported. By 04:14:02 there
are 14.8 such draws per frame. The new captures have only one eligible
holo record and only two captured UI surfaces. Original bytecode from
Effects_Win64_SM50.arc shows this material samples TEXCOORD8 from t1/s1;
the supported A2965EC2931A39C8 variant reads t2/s1. Both have the same
vertex input signature and surface-alpha/glow calculation. Add a t1
coverage variant and carry that slot through transform identity,
content-edit tracking and source capture. The menu overlay-depth fix is
unrelated and remains intact.

Ruled out: the bright particle correction is simply missing its pixel
shader variant. The new run still uses 9AEC596A2B036EA6 /
3789CA2062E196FB. Its emitter remains rigid and all 19 captured
projection/attachment guards pass. Eighteen complete vertex captures
replay through the original VS with zero output error; the final VB was
declined by the existing 32 MiB vertex capture cap. Do not count that
last frame as a successful vertex replay.

The user identifies the faint fleck above the rifle housing. The bright
particle polygons project below that location. Ruled out: the second
EB787F983BC1F5A3 particle batch in the shared vertex buffer is this
fleck; its first 80 vertices are grey world-smoke particles, hundreds of
metres from the weapon.

Confirmed missing attachment material: census draw 387 is a single
triangle with VS 88DCF1164C640EC3 / PS 494506A63091DF8C, using the same
t33/t38/instance buffers as the arms. Its instance stream entry 579
selects pool record 135, bone base 2091. The captured position is
identical to the paired arms' origin (record 133), 49.979 mm from the
source camera in the first frame. The original VS skins the vertices,
loads t33 position, subtracts camera[275], and projects through
cb0[4..7]. The PS samples a 128x128 BC1 texture with emissive scaling.
Ruled out: this shader is an already camera-relative late GUI pass; its
original bytecode explicitly subtracts the camera, and its record shares
the corrected root. Extend the existing attachment family and diagnostic
capture to it. This corrects a proven relative-placement error;
identifying every visible fleck with this triangle still needs visual
confirmation.

Performance prototype: batch all terrain transform work after the 81
captured patch draws, retaining draw-time GPU copies of each constant
buffer. On RTX 5090, 4,017,017 covered pixels, original scene depth,
private coverage and all 81 motion records are identical to the current
path. Twelve alternating samples after warmup measure 1.132 ms/eye
median for the current path and 1.080 ms/eye for the batch; ranges
overlap substantially (1.085-1.792 and 1.046-2.207). This includes the
game's original terrain geometry. Roughly 0.104 ms stereo in this
isolated test is not a credible solution to the reported
multi-millisecond cost, so the prototype remains under build/ and is not
shipped. The flight's actual NVIDIA evaluation averages 1.85 ms per eye
over the session at 4536x4480 output, with the whole temporal bracket
around 2.08 ms/eye. Reducing terrain settings cannot remove that
reconstruction cost. No quality or INI changes accompany these fixes.
Source-copy API contract checked against Microsoft Learn's
ID3D11DeviceContext::CopySubresourceRegion documentation.

Evidence, extracted shaders and isolated prototypes are under
build/review_motion/sep12/flight0413/.

Validation of the added attachment family uses Elite's original
88DCF1164C640EC3 bytecode, the 19 captured pools/bone palettes/ cameras,
and synthetic packed vertices selecting the actual skinned record 135.
The projection matrix is reconstructed from the captured camera rows;
this is a transform-equivalence test, not a raster replay of the
uncaptured triangle. Output displacement agrees with an independent
projection of the measured root delta within 1e-5, with unchanged UVs.
The new snapshot family will capture that triangle's actual vertices,
bindings and draw-time constants on the next dump.

The full NVIDIA SDK build and repository gates pass: 20,924 UI coverage
checks, 213 holo motion checks, 32,899 screen-motion checks, 524 weapon
checks, 403 terrain checks, and the snapshot/config/Python gates. The
original particle replay passes 484,454 checks; the emissive transform
replay passes 819. NVIDIA TAA/DLAA/DLSS integration smoke passes. The
optional D3D debug layer remains unavailable on this machine; functional
GPU output and state restoration are checked explicitly.

## Flight 04:51, remaining pink flicker

Ruled out: adding the 88DCF1164C640EC3 emissive triangle resolves the
remaining pink flicker. The user still observes it on verified build
affebe2, capture 045429. The cockpit UI correction is now confirmed by
the user and should remain intact. Inspect the newly captured triangle
geometry and its screen position before attributing the fleck to it.

The actual triangle uses instance 568, pool record 148, stride-40 packed
vertices, indices 0/1/2 and the captured cb0 projection. Replaying it
through the original 88DCF1164C640EC3 shader places it outside the image
before and after correction. All 19 frames match the independent root
translation within 1e-5, with unchanged UVs (857 checks). This rules out
that triangle as the visible fleck in this capture. Unlike the preceding
synthetic test, this replay uses the actual triangle and projection.

The single complete 9AEC particle frame also matches the independent
correction through the original VS (26,394 checks). The other 18
particle VBs were declined: repeated per-material copies of VB0, already
present in the frame-local mesh table, consumed the 32 MiB diagnostic
vertex budget. Flare vertices alone do not make a complete weapon-effect
frame. Retain that distinction in the reported counts.

Plausible remaining causes and their discriminating evidence:

- Independent point/spot lights (0357BBB2DEE43C1F and 963B52C73B4143AC):
  a pink contribution appears between the before/after copies of those
  draws, with nearby light positions still following the original weapon
  origin.
- Streak/beam effects (E904D334BC8B11EA and 359BF8FF5CFAA4C3): their
  independent model/vertex positions project to the fleck, and their
  individual draw adds it to the colour target.
- The corrected particle pass: the fleck first appears across its own
  draw despite a correct transform replay. Inspect depth/material
  interaction rather than moving the emitter farther.

These are hypotheses, not rendering fixes. Drawstate v6 captures all of
those vertex layouts, instance streams and constants in the same run. It
also brackets each candidate draw in the first source frame with a
native lower-right colour crop up to 1024x1024. The exact draw index,
before/after phase, crop origin, full target dimensions and original HDR
format are saved. R11G11B10_FLOAT is the actual 5120x2880 scene target
in this flight. Crops have a separate 64 MiB limit and explicit
declines; they are not whole-screen captures. Normal play performs no
such copies.

Avoid duplicate mesh VB0 payloads when the full frame-local table
already contains them. Effect input layouts also bound the unused tail
of known instance/nonindexed streams. Keep original binding offsets,
indexed windows and all existing caps. These changes recover diagnostic
space, not rendering performance. The UI and weapon corrections are
unchanged.

Environment: Valve SteamVR through the OpenVR proxy, reported 103x103
degree projection, 4404x4348 submitted eyes and 5120x2880 on-foot
source. The log does not establish the headset model. The capture works
before AA and does not depend on a DLSS model or compositor reprojection
mode. Evidence and replays are in build/review_motion/sep12/flight0451/.

Targeted GPU capture and parser tests verify packed HDR bytes before and
after target mutation, crop coordinates, binding preservation,
first-frame restriction, budget declines, reset, and backward-compatible
reading of older drawstate versions. The exporter keeps raw HDR in the
binary and clips its PNG preview for display; compare raw values for
attribution.

The full SDK build and NVIDIA DLL smoke checks pass, including 20,924
UI, 213 holo-motion, 32,899 screen-motion and 524 weapon checks. The
capture fixture verifies the new writer/parser and exact HDR crop bytes.
No rendering fix for the remaining flicker is claimed from these tests;
the additional draw-time evidence still requires a flight.

## Flight 05:19: point-light fleck and stationary night vision

Verified v0.15.1-37-g20aeb79, build 6AA5346C. Capture 052141 has 19
complete source frames and 114 complete effect snapshots, with ten
before/after HDR images and no declines. The second 0357BBB2DEE43C1F
point-light batch (PS 81812EF97FB4A361, snapshot 62, 201 instances) adds
the thin upper pink highlight: 10,534 changed pixels in the saved crop.
The 9AEC particle draw adds the separate interior glow.

Ruled out: spotlights, the first 512-point-light batch and E904 streaks
cause the captured upper fleck; each changes zero pixels in this crop.
Ruled out: scaling the light's root correction by the projection ratio.
The light uses world near 0.025 while the weapon uses 0.0675, but its
position is scaled about the original arms origin, not the camera.
Undoing that scale about the arms leaves its emitter-local offset stable
within 0.75 mm across all 19 frames. Scaling about the camera instead
produces a 34 mm jump. The required root translation is the full mesh
translation; range and colour must remain unchanged.

Each frame contains exactly one small light in the attachment volume,
with radius 0.025925925 m. The next closest light is over 13 m away. Its
batch index changes, so neither index nor colour identifies it. Apply
the existing validated arms correction to small local lights in a
private instance stream, gated by the verified shader pair and matching
light/mesh camera. Preserve world lights, all non-position fields and
the original buffer. This remains part of fix.weapon_stability.

Capture 052339 shows night vision at 2862x2826 input and 4404x4348
output. The user confirms blur even with head and ship stationary. The
only additional pixel-shader hash relative to the earlier cockpit census
is F786D34B5E118D5E with VS FCF7BD2896751D96, draw DC0 #525. It samples
scene depth and normals, including configurable pixel-block quantization
and an unconditionally quantized centre-normal tap. It does not sample
scene colour. Its actual PS b2 is not in this capture; do not assume the
configured block size or change the effect from shader disassembly
alone. Check source/output crops and coverage next, then capture the
missing constants and pass contribution if needed.

Evidence: build/review_motion/sep12/flight0519/. Environment remains
SteamVR/OpenVR; headset model is not established by the log. Weapon
correction runs before AA and does not change runtime reprojection.

Original 0357 VS stream-output replay passes for both point-light
batches in all 19 frames (2,849,237 checks). The independent expected
stream translates only the identified weapon light. Every other light
and every radius/packed payload byte stays unchanged. Shader outputs
match exactly. The 823-check production regression covers
projection/camera mismatch, freshness, byte ranges, state restoration,
the live toggle and allocation reuse. The bounded private buffer grows
to capacity and does not shrink between the alternating 512/201-light
batches.

Ruled out: the UI mask or fixed DLSS bias spreads onto the night-vision
terrain. The sampled terrain ROI is zero in UI, Bias and UiEdits. The
captured camera dimensions are also exactly 2862x2826 with matching
reciprocals. The user has not yet compared night vision with AA Off.

Add the exact night-vision shader pair to drawstate capture. For this
pair, slot 1 captures PS b1 instead of unused VS b1; slot 3 remains PS
b2. Save its before/after HDR contribution in a central native
1024-square terrain crop during the first watched frame, within the
existing 64 MiB effect budget. The completion log reports zero matches
distinctly from complete constants/images. The GPU fixture and reader
check original PS bindings, crop coordinates, exact packed pixels,
later-frame limits and shader exports. These are diagnostic copies
during an armed dump; night-vision rendering itself has not changed.

Next flight: verify crouching with weapon stability enabled; capture
night vision with DLSS and AA Off from the same stationary cockpit view.
The comparison and actual night-vision constants distinguish effect
sampling from temporal reconstruction without tuning sharpness blindly.

The full SDK build and NVIDIA DLL smoke test pass. The capture fixture
caught the parser's old lower-right-only validation; it now accepts the
exact night-vision pair and its central crop while retaining bounds and
format checks. The complete fixture, Python gates, 20,924 UI checks, 213
hologram checks, 32,905 screen-motion checks and 823 weapon checks pass
on the final source.

## Flight 06:01: stationary night vision and aiming effects

Verified 40174ac, build 6AA53E09. The user confirms the pink rifle fleck
is gone during crouching. Preserve that attachment correction. A
separate blue-white ball appears briefly while aiming down sights and
crouching; identify its draw and pose before assuming it is the same
light.

Ruled out: DLSS alone causes the night-vision blur. The user observes it
with AA Off and DLSS, stationary, and captured both modes in this run.
Captures 060524/060543 contain night vision; 060838/060857 contain the
on-foot aiming/crouching effects. The first pair has complete
night-vision PS camera/settings and four before/after images per dump.

060838 is a controls-menu capture with the on-foot scene behind it;
060857 is the actual aiming pose. In all 19 aiming frames, both mesh and
particle cameras use near 0.025, rather than the hip-fire 0.0675. The
light radius also changes from 0.025925925 to 0.07. The particle guard
therefore declines the aiming emitter while the mesh/light correction
continues, separating their positions by up to 130 mm during crouching.

Ruled out: near 0.025 always means a world particle. In this aiming
capture, the emitter origin matches rigid weapon record 12 within 2.2
micrometres across all 19 frames. Its rigid model and the matched part
stay inside the same validated arms attachment volume. Use this
draw-time association for the aiming projection instead of broadly
accepting nearby world particles. Retain the existing hip-fire path.

Night vision's actual settings are identical across the two captures:
2774x2740 input, PS b2[11]=(40,1,0,0), b2[5].y=1. The optional
pixelation flag is zero and the Sobel radius is one input pixel. The
centre-normal sample still rounds its cell size up to two pixels, but
that alone is not proof of the reported overall blur. The before/after
HDR copies isolate the green terrain detail to this pass. Its normal and
depth inputs are needed to replay and discriminate sampling changes.

The user clarified that only the distant green outlines are fuzzy;
nearby body/cockpit detail looks good, and ground textures are not the
reported problem. They also found Elite's SMAA enabled. The census
confirms its three passes after night vision in both captures: 060524
DC0 #662-664 and #667-669; 060543 DC0 #664-666 and #669-671. These use
VS 68842760565CC3BA, 03D186CE0EC031E3 and 98E6F9986FDC9A53, with the
expected edge, area/search and neighbourhood-blend inputs. EDVR AA Off
does not disable those game passes. It is therefore not an unfiltered
night-vision reference.

Next comparison: disable Elite's SMAA alone, retaining the same EDVR AA
mode, HMD quality and stationary view. Clearer distant outlines would
implicate this extra smoothing pass; unchanged detail would leave the
night-vision sampling/input resolution as the next investigation. The
user is testing this. Do not alter the night-vision shader or add a
sharpening compensation before that comparison.

Ruled out: SMAA is the main cause. The user repeated with SMAA disabled:
the green distant detail remains very blurry in EDVR AA Off and DLSS;
Off shimmers/flickers continuously, while DLSS calms it but still
flickers at points during head movement. Flight 06:26 (same verified
40174ac) contains capture 062922 while moving the head with DLSS. Its
censuses contain no SMAA shader trio. The draw-time HDR crop already
contains the dense green terrain pattern before subsequent
AA/tonemapping, and all 28 night draws retain the same settings and
correct input dimensions.

Issue #25 (Night vision "pulse" line linked to headset movement) is
related at the pass level, not yet established as the same defect. Its
attached 21:46:34 log is release v0.15.1, linked 2026-09-11 02:14:54
UTC; do not treat it as evidence of the latest branch's attachment
changes. Its 21:54:40 census has this exact night VS/PS in both eyes,
DC0 #438 and #491, at 1638x1554 input. The runtime log identifies
SteamVR; DLSS settles on preset K at 65% input. Besides the
normal-gradient detail, PS instructions 393-404 modulate brightness from
sampled depth and PS b2[1].x, with a narrow pulse shaped by b2[10]. This
is a separate candidate for the head-linked pulse. The archive has no
draw-time normal/depth pixels or night-vision constants to prove that
mechanism.

The pulse lead has a concrete depth producer: #25 DC0 #269/#403 fills
the exact t1 resources used by its night draws. Our 060524 DC0 #318 uses
the same PS CB95394B50D737D6 and VS DEF19B035D5EDEDC. The retained pixel
bytecode disassembles to sample hardware depth, add cb2[0].w, divide
cb2[0].z by that result, and cap the result. This is linear view depth,
without a view-ray length factor. The night shader feeds that depth into
its pulse directly. A fixed terrain point's view depth changes with head
rotation; this gives a specific explanation to test for the reported
head-linked band. It does not establish that radial pulse reconstruction
also fixes the separate fuzzy outlines.

Extend the armed drawstate capture to v7: first-frame PS t0..t4 inputs,
both original sampler descriptors and the viewport, plus the night
draw's VB0/index window and input layout. Keep the actual typed SRV
formats, including R32_FLOAT depth/exposure, R10G10B10A2_UNORM normals
and raw BC4 blocks for the small mask. Every eye gets a draw-time copy
even if resources are reused. Native crops retain their origins and
share the existing 64 MiB image budget; the vertex budget remains 32
MiB. No normal-play copies and no night-vision rendering changes.

Discriminate with those inputs: compare the original shader replay to
the recorded HDR contribution, test the unconditional two-pixel centre
sample separately from the eight normal-gradient taps, and isolate the
depth/pulse term. A neighbouring input texel is available throughout the
interior of the native crop; exclude its boundary in replay. Do not
infer blur from exposure-scaled PNG previews alone.

Validation: the GPU fixture covers both eyes rewriting the same depth,
normal, exposure and mask resources; its reader verifies the earlier
pixels survive the rewrites, BC4 block rows are complete, both sampler
filters/LOD values and viewport survive, and the original vertex/index
windows and layout are retained. Missing inputs/samplers and budget
declines are explicit; raw inputs are not exported as colour images. The
reader retains v1-v6 support and rejects malformed v7 records.

User requirement: any eventual night-vision rendering fix must have a
live in-game Fixes toggle for A/B comparison. This diagnostic-only
change does not add a toggle that would have no visible effect. Keep
that requirement with the eventual fix, including its config contract
and state restoration. The aiming emitter remains under the existing
weapon-stability toggle.

The emitter correction now retains the exact rigid-part association when
aiming. It scans the already available GPU instance pool and adds the
same arms displacement as the mesh; no readback, additional render pass
or allocation per frame is introduced. The original particle VS was
replayed over all 38 frames from the two on-foot captures, with the
captured draw arguments and original index windows reconstructed from
their saved offsets. Its corrected outputs match the independently
translated emitter in every frame. Hip-fire behaviour is retained.
## Flight 06:56: landed night-vision pulse and remaining ghosting

Verified 0879045, v0.15.1-41-g0879045, in the Steam install. Captures
065827 and 065830 show head movement with night vision; 065841 catches
the startup pulse while landed. 065915 shows aiming/crouching on foot.
The user confirms the detached aiming ball is gone. Remaining ghosting
has not yet been localized to weapon geometry versus the nearby ammo UI.

Ruled out: the pulse occurs only in flight. The user captured it shortly
after enabling night vision while landed; PS b2[1].x is 962.975 metres.

Ruled out: missing night-vision inputs or reduced-size depth/normal
textures explain these captures. All six first-eye draws have all five
inputs, both samplers, geometry and complete camera/settings constants;
there are no failed copies. Depth and normals are 2774x2740, matching
the input eye. The depth/normal sampler is point-clamp with no mip bias.

Replaying the exact original VS/PS on NVIDIA reproduces 93.7-94.9% of
terrain pixels bit-for-bit; remaining absolute error is 0.58-0.72% of
the added night-vision signal. WARP differs by about 6%, so it is not a
pixel-exact substitute for the captured GPU. A readable HLSL
transcription reproduces the NVIDIA original replay to rounding error.
The capture comparison is close, not pixel-exact, and does not establish
a headset improvement by itself.

Two shader defects are independently testable. First, the pulse uses
linear camera-forward depth, produced by PS CB95394B50D737D6, as though
it were distance from the viewer. A head rotation changes that quantity
for a fixed landscape point. Reconstructing the per-pixel ray from the
actual asymmetric eye projection gives radial distance without changing
the range fade, pulse timing, exposure or normal-edge filter. Second,
instructions 290-303 always quantize the centre normal lookup into 2x2
cells even though b2[11].z disables pixelation on the other 13 lookups.
The cell centre is also a texel boundary for point sampling. Respecting
the existing pixelation flag restores the current pixel's normal.

These are the scope of the proposed live Night vision stability toggle.
They do not establish that every distant fuzzy outline is resolved. The
scalar normal-gradient filter also varies with view direction, but
replacing it with a full-vector gradient changes the intended edge
response and has not been justified as a fix for the reported blur.

Implemented `fix.night_vision_stability = 1`, **Night vision stability**
in Fixes, live and independent of AA mode. Off binds the original game
PS. Only the exact measured VS/PS, 240-index single-instance eye draw
and matching depth/normal/camera resource contract engage. The shader
adds no texture copies, readbacks, extra draws or history surfaces.
Compile failure retains the original draw. Other runtime paths are
unchanged; this is a D3D11 eye-draw correction with no OpenVR compositor
dependency.

Targeted GPU validation: 32,844 checks cover the pulse under rotated
asymmetric cameras, native centre sampling, intentional pixelation,
singular-camera fallback, unknown resource formats, live Off/On,
deferred-context refusal, compile failure and shader restoration. All
six NVIDIA capture replays pass: the test-only stock transcription
matches the original PS replay at 99.9962-99.9984% of terrain pixels;
signal-relative error is below 0.000014%. Fixed output is finite in all
six captures. Headset clarity and residual ghosting remain unverified.

### Weapon history motion

The first 065915 motion input assigns the solid gun housing a median
9.93 input pixels of vertical source motion (10.26 after jitter), while
paired raw C00/C07/C15 crops show the housing holding its screen
position. The ammo UI is already classified separately with
ScreenMotion.w=3. The weapon is w=1 and follows the source camera's
world reconstruction. This treats camera-attached geometry as stationary
scenery while the player crouches or walks, introducing false motion.

The source depth texture already provides a reliable discriminator:
stencil bit 0x10 covers the first-person weapon and arms. The final
5120x2880 source has values 4 for scenery and 20 for the weapon. An
overlay of that bit follows the opaque rifle silhouette exactly,
including the sight housing; it excludes the scenery visible through the
glass. Confirmed on 065915/060857 aiming captures and
060838/052141/045429/041300 hip-fire captures, covering 10.1% and 14.8%
of the source respectively. No colour/depth threshold is needed.

Use that existing stencil plane to retain source UV for first-person
geometry while continuing to project it through the actual previous
screen/eye mesh. This preserves temporal AA and runtime reprojection. It
removes false player-camera motion; it does not manufacture previous
skinned animation poses. Large weapon animations still rely on temporal
disocclusion handling. Keep it under the existing Weapon stability
toggle, and fall back when the source has no stencil plane.

Implemented without another pass or source copy: an optional stencil SRV
shares the already retained source depth texture, with one stencil
lookup per screen pixel. The added t11 binding is saved/restored. The
existing Weapon stability setting controls this with live reload and
survives inactive-screen resource release. The screen-motion GPU suite
passes 51,384 checks, including tagged/untagged surfaces at equal depth,
both eyes, retained previous-screen motion, Off/On, absent stencil,
replaced source textures and restoration of the original t11 binding.

The final full build and all gates pass, including the unchanged 1,080
weapon attachment checks. NVIDIA smoke passes. The generated settings
schema exposes Night vision stability as a live Fixes toggle, grouped
under Night vision. Next flight: compare that toggle while rotating the
head through the landed startup pulse, then crouch/aim with Weapon
stability on to assess residual ghosting. These tests validate code and
captured inputs; neither visual improvement is claimed headset-verified.

## Flight 07:41: aiming regression and unchanged night vision

Verified v0.15.1-42-g68ed193, linked 13:37:34 UTC. New night captures
074418/074420/074423 and weapon captures 074545/074557 are from this
build. The log confirms night vision engaged at 2774x2740 and source
weapon stencil motion engaged before the weapon captures.

Ruled out: fixed source UV is a sufficient weapon-motion model. It
removes false walking/crouching camera motion but ghosts during aiming
and mouse-driven weapon movement. The new user report refutes that
shortcut; the replacement must measure the rendered geometry's own
motion, including animation and projection changes.

Ruled out: correcting pulse distance and centre-normal quantization is
sufficient to fix the reported night-vision fuzziness. Both changes ran
and the user sees no meaningful improvement. Do not re-propose these as
the main blur fix or add sharpening over the unresolved source.

### Rendered weapon motion

The replacement captures original post-VS positions while the attachment
correction's private pool is still bound. It includes the game's own
skinning, aiming projection and animation. Stream output uses the
original vertex bytecode's position signature, avoiding a second
implementation of the packed vertex and bone formats. Point-list capture
preserves repeated and degenerate indices; the motion raster uses the
original triangle list. The pixel shader interpolates previous
homogeneous clip positions with current perspective, producing
previous-minus-current source pixels. ScreenMotion then composes those
with the actual prior curved screen and eye transform. It does not
freeze the weapon in source UV.

The existing Weapon stability toggle controls this, and AA Off allocates
no temporal weapon resources. The successful
attachment/pink-fleck/aiming ball corrections are retained. Runtime
reprojection and presentation timing are unchanged. This path is
D3D11-side; the observed flight uses OpenVR, preset K, 2774x2740 input
and 4268x4216 output per eye, with a 5120x2880 on-foot source. No
headset model or alternate runtime improvement is inferred from these
captures.

Only supported opaque source draws that write the game's stencil bit
0x10 and source depth qualify. This bit also occurs on generic meshes;
it is not a globally unique weapon identifier. Replaying their actual
geometry handles those too. Geometry identity includes retained vertex/
index buffers, layout, shader and draw windows, excluding reordered
instance slots. Ambiguous repeated identities reject the frame's weapon
history. Missing, changed, oversized and newly visible meshes reject
history rather than borrowing scenery motion. Index/vertex writes
invalidate correspondence; bone/camera changes are the motion to track.
Depth equality restricts the motion raster to visible fragments; saved
source depth rejects later occlusion at the screen consumer.

The cache is bounded to 64 records, 131072 indices per draw and 32 MiB
of position buffers. The source motion/depth target costs 112.5 MiB at
5120x2880, with a 128 MiB cap. A 0.5 MiB sequential index buffer permits
both passes to use the original draw entry point. All three are released
when AA/Weapon stability is disabled or the source becomes inactive. No
per-frame CPU readback or source-colour copy is added. Explicit eye
dumps include WeaponMotion beside ScreenMotion, so coverage and vectors
can be checked without another motion-model assumption.

Validation: 57370 production GPU checks pass on WARP and NVIDIA,
covering aiming translation, asymmetric bone movement, projection
changes, perspective interpolation, degenerate indices, live Off/On,
missed frames, duplicate identity, changed indices, size limits and
state restoration. The screen consumer passes 54278 checks, including
composition of animated source vectors into both eyes and rejection when
the new map is absent. The full build, all gates and NVIDIA DLL smoke
pass.

The original VS stream-output probe executes on 228 mesh/frame records,
but these dump tables retain each pool/bone resource once per frame,
before some later viewmodel updates. Their reconstructed positions are
therefore not reliable ground truth for the visible aiming pose. Do not
use the initially calculated 100+ pixel changes as actual weapon motion.
The production capture avoids this limitation by running at the actual
draw. The new WeaponMotion dump records that result directly.

An initial raw-capture timing replay had no visible coverage and was
rejected. A corrected cost experiment repositions four complete captured
meshes in the diagnostic camera, retaining original VS/skinning and
86592 indices at 5120x2880. With 842133 motion pixels, NVIDIA timings
are 0.0134 ms without the added pass and 0.2404-0.2465 ms with it; CPU
submit cost is 0.0004 versus 0.0078-0.0115 ms. This is an isolated RTX
5090 cost check, not an in-game frametime prediction or a complete gun
replay.

### Night-vision component isolation

Replaying the shipped shader with all new draw-time inputs matches the
recorded distant-terrain pixels at 99.9983-99.9991% in the three
left-eye crops. Resource size constants are exactly 2774x2740 and its
reciprocal, so a stale sampling size is ruled out. Component-isolation
replays show the normal-edge filter contributes 95.5-95.7% of the added
distant green signal, surface orientation about 4%, and the procedural
grid none in these regions. This identifies the dominant fuzzy pattern
before DLSS.

The edge detector measures only the camera-Z component of the normal
gradient. Holding the captured gradient fixed while rotating it between
the two captured head bases changes its response by a median 38.1% (95th
percentile 254%). That establishes an orientation-dependent brightness
mechanism, not a complete explanation of perceived blur. A scratch
full-vector-gradient replacement removes this directional dependency but
increases mean response by 76.8% in the tested terrain and leaves the
dense fuzzy pattern. It is not included in the build.

Ruled out: replacing scalar normal gradients with their raw full-vector
magnitude is a sufficient clarity fix; the replay mainly brightens the
same dense pattern. Night-vision rendering remains unchanged this turn.
The next investigation should address generation/filtering of the
terrain normal edges, using the captured inputs, rather than requesting
another identical flight or treating the pulse as the main blur cause.

### Follow-up: geometry contours with no surface fill

The user identified the dense pixelated patch before the mountain ridge
and requested outlines for actual geometry rather than fine surface
detail. Replacing the normal-map edge contribution with depth geometry
removes that dense patch in the same captured inputs while retaining the
ridge, mound and rock silhouettes. This confirms the dominant noisy
signal comes from normal-detail amplification. It does not establish why
the patch's boundary is circular; a specific terrain LOD or material
transition remains unverified.

Ruled out: an unnormalized inverse-depth Hessian multiplied by centre
depth, because near/far discontinuities become excessively bright in the
replay. Normalize by the sum of the nine inverse-depth samples instead.
This bounds the response and makes it independent of absolute scene
scale. Inverse forward-depth is affine on a perspective plane; its
Hessian avoids outlining an ordinary slope. The mixed derivative
includes diagonal contours. It is a geometric edge detector, not an
object-category classifier, so real terrain folds can still get lines.

An additive green surface prototype hid texture contrast. A subsequent
texture-multiplication prototype preserved that contrast, but the user's
final direction was "No green fill please." Neither surface treatment is
shipped. The final geometry path adds no constant fill, tint or
normal-map orientation shading. Between contours, the game's existing
surface colours and texture remain visible. Unlit ground consequently
stays dark. The pulse, exposure, mask and range still control the
geometry contours. Deliberately pixelated and sampled-colour artistic
modes keep their existing shading path.

This stays under the existing live Night vision stability toggle, with
the exact original game shader when Off. It changes only the matched
pixel shader: no blend-state replacement, scene-colour copy, additional
draw, history surface or CPU readback. Its normal terrain path uses nine
depth samples for geometry instead of the nine material-normal samples.
The existing runtime-independent D3D11 eye/resource gates remain.

Validation: 66138 production GPU checks pass on both WARP and NVIDIA.
They cover flat and sloped planes, material-normal detail rejection,
physical depth steps, absolute-scale invariance, extreme near/far
boundaries, exact preservation of surface colour/contrast between lines,
mask/range/on-off behaviour, radial pulse under rotated asymmetric
cameras, deliberate pixelation, compile failure and shader/blend state
preservation. All six NVIDIA replays of 074418/074420/074423 are finite
and retain geometry silhouettes without the dense normal-map patch.
Replay comparisons use a shared exposure scale. Near cockpit differences
are not a full accuracy check because the scratch replay lacks the
original stencil state; the production draw retains it. Headset clarity
and motion stability remain to be validated in the next flight.

The full build, all regression gates and NVIDIA DLL smoke pass with this
final no-fill shader and the rendered-weapon-motion correction.

### Flight 09:15: confirmed fixes; brighter exterior terrain

Verified `edvr_gfx_20260912_091550.log`, v0.15.1-44-g73919ba, linked
15:09:06 UTC. Night vision engages at 2774x2740 and original- vertex
weapon motion engages on the source screen. The user confirms the weapon
fix looks good and likes the no-fill night vision, including its cockpit
exclusion. They request a modest exterior brightness increase and
clarify that this includes terrain between the outlines.

Lift the contour radiance by 25% without increasing contour opacity or
width. Also multiply the existing scene colour equally in RGB, up to
1.25 within the game's mask/range/fade. This is a neutral exposure lift:
there is no added colour, green fill or normal-map surface shading.
Retain original alpha attenuation beneath the contours, the original
depth/stencil state and the pulse. Zero mask, opacity or intensity and
out-of-range surfaces receive no scene brightness increase. Deliberate
pixelation and sampled-colour artistic modes retain their prior output.

Dual-source blending performs the scene multiplication in the existing
draw without copying or sampling the scene colour. The replacement now
requires a single non-MSAA floating-point target 0, matching eye size,
and the measured ONE/INV_SRC_ALPHA RGB blend contract. Changed MRT,
format or blend contracts retain the original game draw. Save/restore
the original blend state, factors and sample mask alongside the shader;
leave the original cockpit stencil state and reference bound. Shader or
blend creation failure retains the original path. The existing live
Night vision stability toggle still restores the exact game shader and
blend when Off. No extra pass, full-size surface or readback is added.

Validation: 79609 production GPU checks pass on WARP and NVIDIA. Added
coverage checks neutral channel scaling and retained texture contrast,
unchanged cockpit pixels under stencil exclusion, stencil/alpha
preservation, zero-intensity behaviour, changed MRT/blend refusal and
blend restoration. The six previous 074418/074420/074423 capture replays
remain finite and show median terrain green-channel brightness ratios of
1.239-1.244 after range fade and HDR rounding. These are replay
comparisons against the accepted no-fill shader, not fresh eye dumps
from the 09:15 flight. Near cockpit pixels are masked in the diagnostic
preview because that scratch replay does not include captured stencil;
the production GPU test exercises the actual stencil exclusion path.

The full build, all regression gates and NVIDIA DLL smoke pass with the
neutral exterior brightness change. The final brightness preference is
pending the user's next headset check.
