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
