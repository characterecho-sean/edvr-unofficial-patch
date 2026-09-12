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
