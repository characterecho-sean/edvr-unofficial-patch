# Shared temporal AA for VR and flat Elite

## Status

- **State:** implementation on `codex/flat-temporal-aa`; do not merge to main
  before qualification. First milestone is a capture-only flat installer,
  runtime scope enforcement and bounded desktop probes. Corrected capture
  passed all 83 build jobs and the 254-key config contract. Epic replay on
  2026-09-24 confirms scaled target families and draw-time b1 evidence. The
  focused camera/handoff replay now contains known motion-family draws and the
  measured tone/copy/UI chain. Passive mono selection now replays all 70 world
  and three handoff records at both sizes; the selector build also passes all
  83 jobs and the 254-key contract. See section 10. Flat temporal
  reconstruction/jitter are NOT enabled.
- **Priority (Sean):** performance over code sharing. Share math/backends where
  cheap; keep separate frame scheduling/capture paths when that avoids copies,
  synchronization or additional per-draw work. Defer broad core extraction
  until flat capture establishes the necessary boundary.
- **Recommendation:** two installer artifacts, one graphics implementation, one
  temporal pipeline, separate VR and mono frame adapters. Flat installs enable
  only temporal AA and its required support services.
- **Open:** flat camera/projection ownership, scene/depth identity, resolve
  boundary, UI ordering, render-scale ownership, and mod hook ordering need a
  focused mono frame contract. The corrected capture sees 1280x720 and 960x540
  scene-target families, the shared 5376-byte VS b1, and a later
  full-resolution panel draw. Registers 270..275 now match the existing
  source-camera encoding on supported shader pairs. Passive resource selection
  works on both captured frames; jitter/inverse consistency, runtime resource
  ownership and backend integration remain.
- **Ruled-out pointer:** the kinematic arc's Status records rejected motion
  estimates and the nonexistent engine velocity buffer. Reuse engine-record
  motion; do not revive estimation or the retired deferred UI replay.
- **Next session:** qualify the paired scene/deferred projection correction and
  remaining consumers before jitter. Avoid another broad discovery flight.
  Later qualify treatment, on-foot scenes and each mod arrangement. Use
  `tools/edvr_log.py` with the actual `--target`, `--expect-build HEAD` and
  `--grep "flat (temporal|discover)"`. No headset is needed for discovery;
  VR still needs regression testing.
- **Test target (Sean):** use the Epic installation for all in-game tests.
  Odyssey is under `C:\Program Files\Epic Games\EliteDangerous\Products`.
  Preserve its existing INI; F10 is the flat default when dump_draws is absent.
- **Compatibility decision:** the prototype accepts an absent profile
  descriptor as legacy VR so manual installations keep working. An existing
  invalid descriptor disables fixes, preserving forwarding/chaining. New
  installers and developer verification require `edvr_profile.ini`.
- **Environment:** initial qualification is Windows, Elite's D3D11 renderer,
  mono SDR output. Record game/patch builds, GPU/driver, display/render sizes,
  window mode, installed mods and backend DLL versions. Headset/runtime are N/A
  for flat. VR regression records the actual runtime/headset/per-eye size.
  Other colour spaces and rendering routes require separate qualification.

## 1. Product and packaging

Build `edvr-flat-installer.exe` and existing `edvr-installer.exe` from shared
installer sources/component catalog. Flat selects a temporal-only profile,
requiring no headset/runtime. Existing features and keys keep their names.

| Component | Flat installer | Existing VR installer |
| --- | --- | --- |
| Shared `d3d11.dll` | Yes, identical graphics payload | Yes |
| TAA, DLAA/DLSS, FSR 3.1 code | Shared implementations | Shared implementations |
| NVIDIA runtime | Existing optional payload/ownership rules | Same |
| `edvr.ini` template | Temporal options and necessary support settings | Existing full template |
| EDHM/ReShade preservation | Existing chain planner and proxy loader | Same |
| EDVR `openvr_api.dll`, OpenXR loader/config | Absent | Existing runtime payload |
| VR fixes, controls, headset UI and unrelated game fixes | Inactive | Existing behavior |

Keep FSR's current D3D11 implementation, shared DLSS capability checks and
fallbacks, and applicable third-party notices. Frame generation is outside this
proposal.

The full installation can also use the mono adapter for positively identified
flat rendering. Exactly one adapter owns a stream; exclude VR mirrors. Missing
XR frames alone do not prove flat mode (startup/loading/paused VR). A flat-only
installation encountering VR rendering stands down explicitly.

## 2. Share the pipeline, adapt the frame source

```text
Existing OpenXR/VR frame adapter     New flat frame adapter
pose, eye projection, Submit        game camera, projection, scene completion
             \                       /
              Common temporal frame/view contract
                             |
     Shared scene capture and motion/depth/reactive preparation
                             |
              Existing TAA / DLAA-DLSS / FSR 3.1
                             |
              Shared output and eligible UI composition
                       /                  \
                VR submission       Flat final game image
                                            |
                                  ReShade final effects
                                            |
                                          Present
```

Wrap the existing `edvrTemporalAa` ABI (`src/d3d11/temporal_pass.h:210`) during
extraction. It accepts textures, geometry, jitter, motion and output size;
`native_temporal.cpp:247-395` supplies VR frame/pose semantics.

Extract incrementally into these responsibilities (names are proposed):

- **TemporalSession:** adapter ownership, device/context and generation,
  immutable per-frame settings, expected view count and discontinuities.
- **TemporalViewState:** one view's previous successful camera/projection,
  history, resources and backend context. A mono session owns one; VR owns two.
  Keep explicit bounded capacity and rejection, not unbounded allocation.
- **TemporalPipeline:** common preparation, backend dispatch, composition,
  state restoration, timings and treatment result.
- **VR adapter:** existing pose transforms, eye pairing, reference-space
  changes, projection-query certification and Submit lifetime.
- **Flat adapter:** qualified main scene/swapchain, game camera, projection
  injection, scene completion, desktop size/format and presentation lifetime.

Extract one-view state from `g_eye[2]` (`temporal_pass.cpp:439`); share shaders
and backend code. Mono gets its own view identity. Keep stereo continuity,
foveation and eye diagnostics behind VR capabilities. Retain bounded storage
with an explicit active-view count.

The shared frame contract must name:

| Group | Required information |
| --- | --- |
| Identity | Session/device generation, render sequence, stable view key, settings epoch |
| Resources | Colour, matched depth, motion inputs and masks; viewport/source rect; input and output extent |
| Geometry | Current and previous unjittered camera/projection, actual rendered jitter in input pixels, depth encoding/planes |
| Interpretation | Motion direction/units, colour transfer/exposure contract, camera-relative motion domains and validity |
| Lifetime | Producer context/thread, completion certificate, resource lease, reset reason |

Replace eye-indexed depth/motion lookup (`temporal_pass.cpp:3393-3496`) with
explicit selected resources and a scene/view key including generation. Equal
dimensions do not identify a camera. Retain/copy inputs before Elite reuses
them; outputs remain leased until consumption and unbound before reading.

Share engine-record joins, shader substitutions and motion-source composition
where flat capture proves matching signatures. Supply actual game-camera motion
and cockpit/world domains; identity head motion alone is insufficient. Unknown
moving/skinned content retains the reactive policy and coverage reporting,
never estimated velocity. See the kinematic arc in Status.

## 3. Flat frame lifecycle

1. Identify the main scene by camera, target/depth relationships and output
   lineage. Exclude reflection/shadow/UI-only/secondary/mod-owned targets.
2. Prepare resources/backend and freeze settings before jitter. Select one
   Halton sample per rendered frame, independent of Present retries. Certify
   the draw-consumed projection and consistent dependent/inverse matrices.
3. Capture that scene's exact camera/depth/motion, including deferred command
   lists if used. Later writes from another camera cannot replace them.
4. Resolve once at certified scene completion, compose eligible UI and hand off
   the output. Restore graphics state; exclude EDVR draws from capture.
5. Commit history only for unique successful treatment. Track presentation
   separately; retries never reprocess the buffer. Define continuity for
   test/occluded/failed Presents, gaps and abandoned frames.

`device_hook.cpp:956-976` forwards Present before boundary work: too late for
flat resolve. Prefer a certified game colour handoff before mod effects.
Pre-forward Present requires proven source lifetime/mod ordering, not merely
hook installation order. Observe Present1 if used.

Begin at native resolution; add upscaling after proving input/output sizes,
final blit, UI ownership and existing spatial-upscale stages. Use the actual
desktop destination and shared backend sizing/floor queries. Flat owns any
residual output scaling. Never silently stack Elite's spatial reconstruction or
reinterpret HMD Quality as a desktop control.

Sean's proposed flat control point is Elite's existing supersampling value,
`SSAAMultiplier`. It is not hooked in the capture build. First compare 1.0 and
a lower setting at the same desktop resolution: record scene/depth/UI extents
and the final spatial upscale. If that controls scene resolution independently,
reuse it and replace the spatial upscale with temporal reconstruction. Avoid an
extra target-resizing layer or redundant resampling just to share VR code.

Keep cockpit holograms/world screens inside reconstruction. Generalize
`ui_layer` only for proved final 2D overlays, at output size without jitter,
preserving EDHM colours/inputs. The VR layer is partly unflown
(`ui-layer-2026-09-23.md`); flat separation needs its own evidence.

Reset on camera cuts, mode/settings/size/format/device/owner changes and gaps.
Release swapchain references before ResizeBuffers; reacquire afterward. Failure
disables future jitter and logs its reason. Already-jittered frames need a
validated single-frame de-jitter/output fallback: flat has no compositor FOV
correction. Keep jitter disabled until fallback and projection rollback are
proven. Never substitute stale history or a black image.

## 4. Temporal-only is an enforced runtime scope

An INI with other switches off is insufficient: three-way merge and mirrored
settings can restore old values. Generate a small versioned runtime component
descriptor from the install plan, separate from editable `edvr.ini`. Both
artifacts carry the same DLL; the descriptor declares the installed scope. Read
it before feature initialization. Keep profile and owned descriptor hash in
`edvr_install/state.ini`; do not turn the whole installer record into a runtime
dependency (it currently promises it is safe to delete).

At initialization/reload, effective features = installed scope intersect
requested options intersect qualified render mode. Gate hooks, patches, workers
and allocations, not just menu visibility. Flat permits temporal
capture/motion/depth/masks/backends/qualified UI and support services
(diagnostics, config, chaining). Extract required dependencies from VR
initialization; unrelated fixes and VR services stay idle.

Missing/invalid new-protocol descriptors disable features with a repair message
while preserving D3D11 forwarding and mod chaining. Both installers, developer
tool and manual packages must supply/migrate it. Existing shipped binaries are
unaffected. This is feature scope, not security.

Keep `fix.temporal_aa` and existing backend/quality settings. Generate flat
defaults/visible settings from shared metadata, retain dormant VR preferences,
and keep `advanced.real_dll` plus logging/recovery settings available.

## 5. Installer, repair and ownership

Current change points:

- `build.bat:1666-1699` creates one installer; generate two resource manifests
  and link the same installer sources with fixed profile metadata.
- `tools/gen_installer_rc.py:281-356` requires the native pair and loader.
  Validate required payloads per profile; flat must physically omit VR assets.
- `src/installer/plan.h:94-105` and `plan.cpp:182-211,465-521` require/install
  the native pair. Make the plan component-driven, retaining existing
  graphics/config/NGX operations and their conflict handling.
- `tools/package_native.py` assumes a native archive. Keep its strict pair
  validation and add equally strict flat validation. Match embedded and loose
  payload hashes for each artifact, including descriptor and notices.
- Extend `tools/install_edvr.py` with the same explicit profile semantics,
  verify-only checks and true no-write dry runs. Installation and verification
  continue to use that sanctioned tool; no ad-hoc file copies.

One game directory has one active EDVR installation and one chain record. Add a
schema/profile/component inventory to install state. An old valid installer
record without a profile migrates as the existing VR edition; actual
files/hashes still determine ownership. Missing state is not evidence that
arbitrary files belong to EDVR.

| Operation | Required result |
| --- | --- |
| Fresh flat | Graphics/temporal files only; no Openvr directory creation or runtime validation requirement |
| Flat update/repair | Preserve flat scope, edited temporal settings, mod chain and ownership; select flat release artifact |
| Flat to VR | Install/validate native pair, back up the stock runtime through existing rules, preserve temporal preferences |
| VR to flat | Explicit profile conversion; restore the original VR library and retire only verified EDVR runtime assets |
| Flat uninstall | Restore chained graphics proxy; remove only owned files; leave stock/foreign VR files untouched |
| Mirror recovery | Restore preferences/ownership; regenerate the descriptor for the requested profile, never an old mirror's scope |

Unprovable VR originals/backups block conversion with a concrete conflict.
Updates never switch editions; conversions appear explicitly in the plan.
Commit/rollback metadata and payloads together, retaining backups until commit.
Release asset selection/help must distinguish editions; audit updater names.

## 6. EDHM and ReShade remain part of the contract

Reuse `plan.cpp:307-402` for foreign-proxy preservation and `plan.cpp:683-717`
for restoration. Keep `d3d11_proxy.cpp:189-267` loading the chain outside
DllMain, its system-export fallback and recursion protection. Do not introduce
a competing `dxgi.dll`, rename mod configuration files or overwrite a foreign
proxy. Preserve a user's existing dxgi-based ReShade arrangement and qualify
that ordering separately.

Required image order: EDHM-modified game shading, temporal resolve, separable
final UI, ReShade final effects, Present. Instrument ordering and resize
lifetimes; loader chaining alone does not prove either. ReShade grain/overlays
must not enter history. Qualify depth access against upscaled output.

The existing installer supports one direct chain target (`plan.cpp:345-360`).
EDHM plus ReShade together continue through their own existing chaining
arrangement; this proposal does not invent an arbitrary multi-proxy loader.
Test clean, EDHM, ReShade and that combined arrangement separately.

## 7. Evidence and delivery gates

Collect these together in one discovery build. Every summary prints patch and
game build, profile, render owner and backend availability; an explicit
zero/decline reason distinguishes an unused probe from success.

| Question | Discriminating evidence |
| --- | --- |
| Which image is the flat scene? | Camera/view key, RTV/depth IDs, formats, dimensions, viewport and lineage to main output |
| Where can jitter enter? | Projection owner/write sequence, exact draw-consumed matrix, inverse consistency and rendered displacement |
| Is motion transferable? | Depth clear/encoding, camera-row provenance, matched record IDs, motion-source coverage and residual reprojection error |
| Where is scene completion? | Final scene write, UI draws, copy/blit chain, ReShade entry and Present order on one timeline |
| Who owns scaling? | Scene/output extent, spatial-upscale draws, backend accepted sizes and UI target extent |
| What breaks continuity? | Menu/cockpit/on-foot/map transitions, resize, alt-tab, camera cuts, duplicate/test Present and device changes |

Verify logs with `tools/edvr_log.py --expect-build HEAD` and the actual target.
Record hardware, backend versions/sizes and fixed two-view limits. New parsers
belong in `tools/` with self-tests/build gates.

Delivery order:

1. Qualify flat capture with AA disabled; record confirmed/rejected candidates.
2. Extract shared state behind the VR wrapper; run existing rigs/full build and
   qualify VR image/motion/timing parity.
3. Enable mono native-resolution treatment with certified jitter/fallback.
   Qualify cockpit, world, menus, maps and on-foot; then add render scaling.
4. Test both profiles: install, repair, mirror, conversion, rollback, uninstall
   and verify-only, including foreign DLL conflicts. Dry-run writes nothing.
5. Qualify backends/mods, resizing, AA-off and flat without VR installed.
   Publish each artifact only after its gates pass.

Automate one/two-view isolation, camera/projection association, duplicate
rejection, resource/reset lifetime and profile enforcement with stale INIs.
Extend installer chain tests at
`tools/installer_test/installer_test.cpp:553-577,683+`. Every C++ change builds
through absolute `build.bat`. Flat quality and VR parity remain measured gates.

## 8. Epic capture journal, 2026-09-23

The Epic log `edvr_gfx_20260923_202701.log` matches branch commit fc5f5353
(v0.17.0-489-gfc5f5353). F10 rearmed at 20:28:57.070. Output was 1280x720;
draw, depth, copy and dispatch hooks ran on the owned thread with zero foreign
calls or unknown command lists. Sean ran 0.75x supersampling in a separate
session; compare that log independently, never infer a mid-session change.

- Ruled out: the highest depth-draw target as scene colour, because the
  20:29:02.107 sample reports colour=null, target=0x0, viewport=1024x1024. A
  shadow pass is possible but not proven. Inventory colour/depth targets and
  their output routes before choosing the world scene.
- Ruled out: same-frame latest CB bytes as target-consumed projection, because
  candidate draws span q101..259 with VS BA415283FF452DB2, while reported CB
  write/draw q594/601 uses VS DEF19B035D5EDEDC. Freeze bytes and provenance at
  the actual target draw; later reuse must not replace them.
- Incomplete evidence: the 256-edge table overflows (61 at the first useful
  sample, 2560 cumulatively by 20:29:27). Reserve output-edge evidence and
  report truncation explicitly; missing routes cannot certify separation.
- Startup capture exhausted 12000 Presents in four seconds without draws. F10
  successfully restarted collection. Preserve a bounded window while
  distinguishing startup Present traffic from useful rendered frames.

Next probe must show a bounded target inventory with dimensions, draw-time
snapshots of observed bound CB bytes, and retained output lineage in the same
sampled frame. Reused-buffer, depth-only and full-table regressions must pass
before the next Epic run. AA, jitter and render-scale writes remain disabled.

The separate 0.75x session, `edvr_gfx_20260923_203031.log`, also matches
fc5f5353/build 6AB48917. F10 rearmed at 20:32:27.646. At 20:32:52.764, frame
31384 reports a colour/depth target and viewport of 960x540, format 23, with
two draws q115..130; output remains 1280x720, format 28. This is exactly 0.75
of each output dimension. VS b0 is null on that candidate, so camera ownership
remains unknown. A matched DSV clear at q110 has depth=0; the DSV view and
depth resource naturally have different addresses. A colour copy at q120 is
observed, but the complete route to output is not established.

This supports testing the existing supersampling control for flat render scale.
It does not yet prove the candidate is the world scene or identify the
setting's write owner. Do not hook or enable scaling based on the ratio alone.

Code cross-check: `binding_shadow.h` and the VS constant-buffer hook already
track scene constants in VS b1. The initial flat probe inspected only b0;
therefore a null b0 is not evidence that a draw lacks camera constants. The
corrected capture must snapshot both b0 and b1 at the draw and print the slot
with its provenance. Existing VR f932 view rows remain a comparison point, not
a flat-camera certificate.

The corrected probe keeps up to 128 target pairs, 256 general edges and 64
reserved direct-output edges per frame. It captures at most 32 large and 32
small constant buffers and freezes up to 4 KiB per target/slot exemplar for VS
b0 and b1. Unsupported writes invalidate the CPU shadow. Snapshot storage is
static, and frame reset invalidates metadata without clearing all payload
bytes. The capture ends after 120 seconds or 12000 frames containing draws;
startup Presents are counted separately. ClearState resets the viewport.

Shader-input routes cover shadowed PS slots 0..3; implicit alias unbinds and
higher slots are not observed. Frozen bound bytes do not prove shader reads.
Truncated tables, missing snapshots and absent routes remain inconclusive.

## 9. Epic capture, 2026-09-24: scaled scene and handoff candidates

`edvr_gfx_20260924_050253.log` matches 2f9c5bdb, version
v0.17.0-490-g2f9c5bdb/build 6AB49053. F10 windows start at 05:04:35.492 and
05:05:18.661. The same scene-target family changes from 1280x720 to 960x540
while the swapchain stays 1280x720. This supports retaining Elite's
supersampling as the input-size control; no setting write is implemented.

At 05:05:23.672, frame 36282, all target/edge tables fit (17 targets, 117
general edges, three output edges). The CB pool drops 27 observations, so
missing CB evidence is inconclusive. Failed/test Presents, foreign-thread calls
and unknown command lists remain zero.

- Format 23 colour with the scene-sized depth takes 66 draws q301..413. VS b1
  is 5376 bytes; observed write q297 precedes its frozen draw q301. f932 view
  rows are populated; a projection-like shape begins at byte 3792.
- The same depth also serves format 26 colour, 64 draws q436..618. Its b1
  exemplar at q476 uses VS 68DDDEF04D9894AF, with a slightly different camera
  from the earlier target. One frame need not have only one camera.
- Observed PS-binding edges connect format 23 to format 26 at q436..437, format
  26 to format 27 at q636 (VS F9CFC798F21E9AEA), then format 27 to the
  full-resolution backbuffer at q639 (VS 20F383BBAC05C031). The later panel VS
  A888D51024D9798E draws to the output at q644.
- The matched scene DSV clear is depth=0 at q295. Its identity is shared across
  the format 23/26 targets; encoding and projection still need proof.

Ruled out: treating the format 23 highest-draw candidate as the final scene
colour, because later format 26 and format 27 passes feed output. Its first VS
FC1193AFFC596F74 is already documented as a fullscreen stencil triangle in
`per-object-motion.md`, not proof of world-camera consumption.

Ruled out: the 4096-byte snapshot proving compatibility with engine motion's
camera contract, because `engine_velocity.cpp` reads registers 270..275 at
bytes 4320..4415. Those bytes are outside this capture. f932 and the
projection-shaped block alone cannot replace that evidence.

The first 1280x720 candidate's byte-3792 diagonal is 0.139315/-0.139315,
whereas later snapshots show 1.8495/-1.04034. This block varies by write; do
not treat its shape as a single authoritative projection for the frame.

The next passive contract probe must capture these camera rows at actual known
motion-family draws, VS/PS identities and depth metadata at the observed colour
handoff, and ordering relative to late UI. Keep it bounded, without jitter or
treatment. Reuse pure family/math helpers where useful; the eventual mono
adapter should pass explicit colour, depth, camera and input/output sizes to
shared backends, rather than pretending to be a VR eye.

Reuse boundary: `engineVelocityNoteSource` and `engineVelocitySourceViews`
already maintain mono source motion in SourceEye 2, including the matched
depth, object-record pool and current/previous scene constants. A future flat
adapter can name this source directly after qualification. Its present caller
is the VR on-foot `screenMotionSource` path; do not enable that path's panel
sizing/UI/weapon behavior to obtain motion. Decouple producer readiness from VR
backend warm-up and measure the cost of MRT6 and snapshot work.

Next flight can stay at 0.75x SS: the scale comparison is already recorded. Use
the same Epic cockpit, F10, and a brief camera movement followed by a steady
view for about 20 seconds. The focused probe must distinguish the actual
motion-family camera from the first fullscreen draw's constants and record the
pixel shaders and bound sources at the colour handoff together.

Focused probe design: retain 224 world records and reserve 32 for format-27
screen/output handoffs. Each key includes target/depth, VS/PS, b1 identity, all
96 camera bytes, first-viewport fields/count, and PS0..3 view/resource
identities. Identical keys coalesce with first/last draw and write provenance;
different camera bytes remain separate. Copy the sparse camera slice at the CPU
write and into newly admitted records only. Report missing/invalid/stale camera
writes and per-bank drops distinctly. The family lookup is pure and does not
start the engine motion producer. AA and jitter remain disabled.

Validation: the focused MSVC compile/self-test and full build passed (80
parallel jobs plus three quiet jobs, 254-key config contract, installer
resource checks). Regressions cover the exact 4416-byte camera boundary,
old/later writes, immutable buffer reuse, distinct cameras/shaders/sources/
viewports, and handoff retention when world records fill. Runtime hook,
invalidation, report and reset paths were traced before the next Epic run.

## 10. Epic focused contract replay, 2026-09-24

Both `edvr_gfx_20260924_053124.log` and `_053756.log` match 37062878,
v0.17.0-491-g37062878/build 6AB50936. In the latter, frame 36865 at
05:40:20.640 has 960x540 scene targets and 1280x720 output; frame 40611 at
05:41:02.429 has 1280x720 scene/output. Both have 26 declared pool-family draws
with same-frame camera rows, no world/handoff or large-CB drops. Small-buffer
drops remain explicit (27 and 30 respectively).

The measured camera has zero clip-Z coefficients in rows 270..272, row
273=(0,0,0.0250000004,0), row 274 equal to the clip-W column, and finite camera
position in row 275. This matches the existing mono source's infinite
reversed-Z encoding (`screen_motion.h`). The same camera bytes reach the tone
and output-copy records; the later panel uses different rows with near term
0.10001. Preserve those distinct cameras.

At 960x540, tone VS F9CFC798F21E9AEA / PS FEE777E92850B390 writes format 27 at
q511, sourcing the format-26 colour through PS1. Copy VS 20F383BBAC05C031 / PS
DED8796049C7BB4A samples that texture through PS0 into output at q515. Panel VS
A888D51024D9798E / PS 015EF9349EC097E8 follows at q520. The 1280x720 sample has
the same chain at q939/943/948. Known geometry uses a scene-sized format-19
depth.

Ruled out: all 26 declared-family draws being supported motion writes.
EB5234DB6ADB491D/B7D50283329322C3 and DE545DC8EE4FBB87/91F8937EDA723663 occur
but are outside the existing VS/PS support table; the latter is already a
documented refusal. Reuse pair-level admission and preserve rejection, rather
than broadening it.

Build the mono input selector around the supported scene camera/depth and the
observed format-26 -> tone -> output resource identities and order. Reject
ambiguous cameras, unsupported pairs as naming sources, missing provenance,
wrong extents/viewports, broken lineage and truncated evidence. This can be
tested from captured values before another game run. Selection alone does not
certify shader-read completeness, jitter/inverse consistency, GPU motion
production, resize/history continuity or mod order.

Complete-record replay, beyond the minimal fixtures, ruled out every HDR draw
using viewport depth range 0..1: records 34..36 at both sizes have full XY
viewports and MinDepth=MaxDepth=0. At frame 36865 they occur at q376/379/380;
at frame 41961, q772/775/776. The latter two have the current camera and 6655
and 19 instances. Permit these observed HDR ranges while keeping source, tone
and output-copy viewport validation at 0..1. After that correction, all 70
world and three handoff records select the expected mono inputs for frames
36865 and 41961 (960x540 and 1280x720). Every camera-bearing record (64 per
frame) reconstructed from the log's float text reproduces its exact 96-byte
hash. Both select 21 supported pair draws and count five unsupported draws;
late output starts at q520 and q946 respectively. The regression fixtures
include the collapsed-depth HDR cases and keep tone/copy validation strict.

`flat_mono_frame.h` is a passive report-time selector: it owns copied camera
rows, treats resource pointers as frame-local identities, and creates no GPU
resources or COM ownership. Every report prints a selected/refused verdict with
a reason. The producer and tests share the unchanged shader-family table;
metadata selection neither enables its patches nor certifies motion output.
Full build `build/flat-mono-selector-final-build.log` passes all 83 jobs, the
254-key config contract and both installer payload gates.

The jitter audit establishes the forward transform: for rows 270..273, `row.x
+= (2*jitterX/width)*row.w` and `row.y += (-2*jitterY/height)*row.w`,
preserving z/w and rows 274..275. Ruled out: changing only row 273, because its
captured w is zero; projection w comes from rows 270..272.

Offline bytecode from the saved Steam dump matches all four shader names under
the repository's FNV implementation; game testing remains Epic-only. Pool VS
EB5234DB6ADB491D instructions 103 and 136..141 use b1[275] and b1[270..273].
Deferred VS 7E38A6AA1269C901 instructions 3..5 instead form the view ray from
b2[14..16].xyw dotted with (u,v,1), without reading b1. PS 7CECABDE34FFBE9E
normalizes this ray at instructions 37..39 and uses it for lighting at 48 and
53..54; its b2[2..4] transforms normals, not projection. Tone VS
F9CFC798F21E9AEA has no CB reads. The matching tone PS was not found.

Ruled out: b1-only jitter preserving deferred lighting, because its view ray
comes from separate b2 constants. For this deferred pair, also subtract
`row.x*jitterX/width + row.y*jitterY/height` from b2[14..16].w, leaving its
fullscreen position, sampling UV and normal basis unchanged. This evaluates the
ray at UV minus jitter. The actual Epic b2 contents, UV mapping and camera
relationship still need qualification, as do other scene consumers. These four
shaders do not establish whole-frame coverage.

The wider offline audit covers 71 selected-depth/tone records (117 draws), with
86 exact-hash shader artifacts available and 13 missing (plus null PS). Two
more matching artifacts occur only in the coalesced target summary. Ruled out:
the b1 plus deferred-ray corrections covering the scene. Actual draws also
project through b0[4..7] (0EE43D81E394E70C, CFCA and 2CECEC families) and
b2[10..13] (0357BBB2DEE43C1F, 8289669D93A18C1D, 963B52C73B4143AC). Sky VS
F8FA801F2CB1E27C uses inverse b2[11..14] at instructions 0..4 and emits the
original fullscreen position at 8; its inverse needs the corresponding
clip-offset correction. Radar families also use b2[6..9] and b2[7..10], so
shared scene depth is insufficient to authorize jitter on every b2 matrix. PS
7EAC71963E66C5FE in the target-summary pair reconstructs from SV_Position using
b2[0..2,4] then b2[7..10]; its two draws lack individual contracts.

Missing VS bytecode: 1F3AD1584D7FA3C8, 4D516EF05C68FFA5, 6041FD2D3D0164E1,
8BD7C37ABCEE7E45, 94D5C556DFD6D705, BBAD1CA808E1E292. Missing PS bytecode:
147E748F4CD3AE9A, 188A933094FB422A, 4E4FF61E8A08FC7E, 94676B1FD0DF150F,
BA65C50BBA1ECCBB, E54F2A902E5631F6, FEE777E92850B390. Generated listings and
coverage detail are in ignored `build/flat-audit/`. Next evidence is narrowly
defined: these shader bytes, current b0/b2 matrices and camera ownership,
world/UI classification, and relevant screen-coordinate/depth consumers. No
broad discovery flight is needed to repeat established frame selection.

Runtime integration needs complete persistent CB write tracking independent of
bounded discovery, with substitutions limited to qualified scene consumers.
Resources and backend readiness must be checked before jitter starts. A failed
backend after rasterization needs a validated single-frame de-jitter output
path at the tone/copy boundary, history reset and subsequent stand-down;
restoring a binding alone cannot undo jitter already rendered into pixels. The
next focused qualification must cover displacement/sign at both sizes,
forward/inverse agreement, world coverage, unchanged late UI and injected
backend failure. No additional general capture is needed for frame selection.
