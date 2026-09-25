# Shared temporal AA for VR and flat Elite

## Status

- **State:** implementation on `codex/flat-temporal-aa`; do not merge to main
  before qualification. Main `28ee5f73` is merged as requested (section 19).
  Section 27's solar/smoke bindings establish the earlier star scene, and
  section 26 rejects scene-camera equality as a jitter precondition. Section 28
  implements experimental live jitter with scoped projection bindings and
  warm-up/recovery policy. Latest flight `6e9bde74` accepted 1,135 HDR image
  continuations but no jitter (section 31). The remaining input refusal names
  exact screen-space PS `FCFAD73924BF45B9` with no b1 camera; its newly
  captured bytecode does not consume b1. The camera requirement needs a narrow
  correction for that verified pair. Section 32 implements the actual-binding
  guard and separates geometry camera provenance from camera-independent image
  writes. All 20 captured shader pairs are classified: 19 projection recipes
  and one unchanged image pass. Full build, 79 test jobs and all gates pass. A
  clean-stamped build will be installed on Epic for the next qualification.
  Earlier flight `ce715126` confirms the key fix and captures the HDR copy's
  real image connection (section 30). Every input write is checked before HDR
  continuation. Two older shader pairs still lack creation bytes; F10 captures
  newly observed unknown pairs without a hand-maintained list.
- **Priority (Sean):** performance over code sharing. Share math/backends where
  cheap; keep separate frame scheduling/capture paths when that avoids copies,
  synchronization or additional per-draw work. Defer broad core extraction
  until flat capture establishes the necessary boundary.
- **Recommendation:** two installer artifacts, one graphics implementation, one
  temporal pipeline, separate VR and mono frame adapters. Flat installs enable
  only temporal AA and its required support services.
- **Open:** reported star-corona smearing during camera turns, projection
  jitter, in-game backend qualification, rendered inverse/depth consistency and
  mod effect ordering. Captured mono frames establish scene/depth identity,
  camera encoding, tone/copy handoff and later panel ordering at 1280x720 and
  960x540. Projection algebra is verified offline; rendered consistency and
  safe failure after jitter still need qualification.
- **Ruled-out pointer:** the kinematic arc's Status records rejected motion
  estimates and the nonexistent engine velocity buffer. Reuse engine-record
  motion; do not revive estimation or the retired deferred UI replay.
- **Next work:** qualify the live flat integration, reusing VR's shader
  knowledge and shared motion/backend math without requiring local matrices to
  match the scene camera. The focused solar/smoke binding question is answered
  for this scene; do not repeat the same flight to fill untouched material
  constants or camera-equality labels. Next Epic run: F10 on the main-menu ship
  view for 20-30 seconds, then F10 in flight for 20-30 seconds; inspect applied
  jitter, continued history, HDR input witnesses and unknown pairs together.
  Missing recipes still reject jitter warm-up; do not claim visual
  qualification from preparation counts. No headset is needed; VR still needs
  regression tests.
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

The flat render-scale control is Elite's existing supersampling value,
`SSAAMultiplier`. Epic captures at 1.0 and 0.75 confirm scene/depth dimensions
of 1280x720 and 960x540 respectively, with the desktop output remaining
1280x720. The capture build observes this setting's effect without writing it.
Runtime integration will consume those actual dimensions and replace the
qualified spatial handoff with temporal reconstruction. No extra
target-resizing layer is needed for the measured route.

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

## 11. Focused projection capture, 2026-09-24

Prepare one Epic run to collect the missing shader bytes and actual projection
buffer ownership together. The flat profile now admits only the 13 missing
stage/hash pairs listed above, once per device, through the existing shader
dump path. It logs armed, attempted and succeeded/failed separately. Existing
files must match the creation bytes exactly; partial or corrupt files cannot
count as successful evidence. Admission tests and a harness for actual writes,
existing-file verification and failure paths pass. General shader dumping is
not enabled.

The companion records VS b0[4..7], VS b2[0..16] and PS b2[0..16] from observed
CPU writes, with exact buffer identity, byte hash and write timing. Format-60
scene records are now retained individually. Startup uses compact summaries;
manual F10 permits two full detailed reports, with one bounded fallback if
selection refuses. Repeated refusals preserve the last selected sample.
Projection payloads deduplicate with a cap of 128 per report and explicit
overflow counts, carrying exact uint32 bits as well as floats. The small-buffer
bank grows from 32 to 128 based on the observed at-most-62 distinct buffers per
frame; the large-buffer bank remains 32. Bindings are stage-specific, observed
through forwarding setters rather than GPU readback; unknown, unbound, missing,
invalid, stale and short writes remain distinct. A single 0.75x SS session is
sufficient for this evidence; the existing native/scaled comparison need not be
repeated. AA and jitter stay inactive during this capture.

Validation: targeted collector, hook and regression builds pass, including
exact-byte shader-file checks and both complete prior flight replays. Full
`build/flat-projection-capture-build-retry.log` passes all 83 jobs, the 254-key
contract and installer payload gates. The first full attempt stopped at the
existing wall-clock-sensitive run_jobs self-test's start-order assertion; that
check passed alone and in the full retry without changing its source.

## 12. Focused Epic replay, 2026-09-24

`edvr_gfx_20260924_063611.log` matches 9173f17f/build 6AB5182B and stays under
889 KB. All 13 requested shader captures succeeded. F10 at 06:38:22.974 emits
selected detail frames 71751 and 72201, both 960x540 scene to 1280x720 output,
near 0.025, 21 supported and five unsupported motion-pair draws. All observer
drop counters, unknown lists and foreign-thread counts are zero. The detail
reports contain 125/124 relevant records and 31/34 unique projection payloads,
with zero payload drops and two format-60 draws each. Exact byte counts, FNV32
hashes and before-draw write provenance validate for all 134/140 referenced
projection bindings. Missing material-buffer writes remain explicit; the
capture cannot distinguish static initial data from writes outside the frame.

The newly captured tone PS FEE777E92850B390 samples HDR t1 at unchanged UV and
the colour LUT at t0; it has no depth/projection reconstruction. The second
format-60 PS B403F48CB35D9739, found and hash-verified in the saved corpus, is
just `ret`, so its bound b2 is inert. The actual inverse pass uses independent
PS b2; binding equality between shader stages must not be assumed.

Ruled out: jittering only SV_Position. VS 1F3AD1584D7FA3C8, 4D516EF05C68FFA5
and 8BD7C37ABCEE7E45 also export clip xyw as ordinary varyings; their PSs
derive depth-sampling UV from those varyings. Upstream matrix correction
preserves both outputs. VS 6041FD2D3D0164E1 has the analogous b1 path. Further
consumers needing explicit treatment are the projected sprite VS
94D5C556DFD6D705 (depth tests and b1[281] remapping) and PS 4E4FF61E8A08FC7E's
SV_Position-based light-cluster lookup. New shader listings are in ignored
`build/flat-audit-current/`.

Numerical replay establishes the common camera: deferred b2[10..13] spatial
coefficients equal the b1 transpose exactly, with translation residual below
1.17e-6. Deferred UV rays match the inverse camera within 1.56e-7; sky inverse
times forward differs from identity by at most 1.11e-7; the format-60 screen
inverse residual is below 8.11e-8. Sky/shadow coordinate-basis relations stay
fixed across camera motion within 7.65e-8. Embedded HUD b2 matrices factor into
the same world camera and a stable local transform (rotation difference below
1.1e-7); blanket exclusion of that geometry would be wrong. The later A888
output panel has a separate camera and near term 0.10001. Short-range buffers
contain their complete actual contents; all observed major inverse owners have
complete current-frame data. Generated numeric evidence is in ignored
`build/flat-projection-flight/`.

`flat_projection_math.h` implements pixel-jitter conversion and the five
verified forward/inverse layouts without allocations or D3D dependencies.
Updates reject non-finite inputs and overflow without partial writes. Geometric
tests use independently captured forward and inverse matrices to check
requested displacement, reconstruction, depth/W preservation, exact unchanged
fields and zero-jitter identity. The actual fixtures are from frame 71751;
tests at 1280x720 also exercise size algebra without claiming another live
capture. Targeted compilation, independent fixture review and full
`build/flat-projection-math-build.log` pass (83 jobs, 254-key contract). No
runtime hooks or AA activation are changed by the math component.

The light-cluster dependency remains separate. PS 4E4FF61E8A08FC7E uses
floor(SV_Position.xy / integer tile width); b1[227/228] hold grid dimensions
and log-depth parameters, not an additive XY origin. Four saved compute
lighting variants also reconstruct rays from CS b0[10..12]. Their resource
association with the current game's grid must be established before choosing
between correcting the grid or its lookup. Do not nudge unrelated constants or
treat a saved shader as proof of an active dispatch.

The next focused probe samples actual dispatch bindings during two manually
armed candidate frames. Read back the consumed structured bounds immediately
before their dispatch using queued staging copies, an event query and later
nonblocking polls. This replaces the proposed persistent CPU resource cache: it
measures the actual GPU input without keeping a 32 MiB mirror or tracking every
resource creation. No jitter or AA treatment is enabled by the probe. Sparse
samples are twelve 32-byte records per bounds view, covering four XY corners at
the first, middle and last depth slice. Validate integer grid dimensions, view
ranges and byte arithmetic before submitting copies. Retain the originating
frame, dispatch, constant-buffer snapshot and view range until completion; a
later mono selection cannot certify an earlier capture.

The bounded association capture also records the eight audited compute hashes,
their SRV/UAV links, independent pixel-stage grid constants, and stock
sun-glare vertex-stage depth/viewport inputs. Missing bindings, unavailable
readback, timeouts, capacity limits and frames without a selected scene must
produce explicit results. Polling must continue after the diagnostic window
ends, without waiting for the GPU or releasing driver resources from
loader-lock teardown. Runtime admission still requires the numerical grid/basis
comparison and rendered qualification after this evidence is collected.

The implemented probe reserves eight dispatch records per audited hash, four
graphics records per pixel-cluster/vertex-flare role and two bounds versions
per bounds consumer. Missing CPU constants get one full-buffer fallback per
hash/graphics role per sample; the queue holds at most twenty CB jobs and eight
bounds jobs across both samples. Sun-glare capture includes b1 rows 281 and 332
plus VS t0. CPU shadows check the exact 4512/5328-byte boundaries. All capture
commands bypass the game's observation counters, and the inactive capture guard
returns before its internal TLS check.

Targeted compilation and the integrated flat rig pass. The actual WARP readback
harness verifies snapshot-before-mutation, sparse packing, complete CB
fallback, immutable tokens, internal guards, fixed capacity, cancellation,
timeouts, counter regression and owner handoff. Review corrected starvation
between shader families and a bounds/CB completion-order dependency. The full
build passes in `build/flat-compute-capture-build-final.log`: 83 jobs, the
254-key contract and both installer payload checks. Earlier launcher attempts
stopped at batch argument handling and a Windows PATH-casing issue; the final
run uses the established normalized-environment absolute-path launcher.

## 13. Epic light-grid qualification, 2026-09-24

`edvr_gfx_20260924_072618.log` matches 88a04caa, version v0.17.0-495-g88a04caa,
build 6AB52425. The 172 KiB log contains selected frames 66884 and 67330,
960x540 input and 1280x720 output. Each captures six audited dispatches, one
clustered pixel draw and one sun-glare draw. All capacity and identity drops
are zero; four sparse readbacks complete successfully. All twelve 480-byte
compute CB payload hashes match current-frame CPU writes.

Both samples establish the same chain: 593EA u0 feeds 074CB t0; 074CB u0 is
76BFC u0, F7CED t0, lighting t0 and PS 4E4FF t0. F7CED's worklist outputs match
lighting t2, and its argument buffer drives the two indirect lighting
dispatches at offsets 0 and 12. The active lighting variants are 599814 and
EB0245; both write the selected HDR. Their R32 t3 depth is also sun-glare VS
t0; their stencil t4 view references the selected scene depth. Sun-glare
b1[281] is the full viewport, and b1[332] contains the measured 960x540 size
and reciprocals. Bytecode establishes positive view-Z interpretation of the R32
texture. Its specific producer and behavior under changed projection remain
unmeasured; consumer association alone is not producer proof.

The coarse grid is 1x1x32 and the fine grid 8x5x32, with 120-pixel tiles.
Captured bounds remain bit-identical across camera rotation. Their eight
frustum corners reconstruct from the captured view-space rays over padded
960x600 to maximum relative error 1.53e-7. Ruled out: clamping the final tile
to 540 pixels, because that contradicts the GPU bounds by 1160.53 at the far
slice. Exact raw bounds now form regression fixtures in the flat test rig.

No cluster lookup rewrite is needed for the existing eight Halton phases: at
raster centres p=n+0.5, p-j remains within the same pixel, and hence the same
120-pixel tile. The 8x8 work groups divide that tile exactly. A tested
1/16-pixel float32 margin restricts admitted offsets to +/-7/16, includes every
existing phase and preserves tile indices through the largest D3D11 texture
dimensions. The existing bounds/worklists can therefore remain unchanged under
this contract, while CS inverse rays receive the verified correction. This is
not permission for larger jitter, multisampling or a different grid layout.
Rewriting pixel lookup alone would not repair their worklist/culling coverage.

The next implementation uses a lean mono adapter and explicit camera/depth/
engine-motion inputs to shared backend wrappers. It replaces PS0 only around
the qualified original output-copy draw, then restores game state; later UI
remains outside temporal history. First qualification uses zero jitter, with
original-copy forwarding and history invalidation on any refusal. This tests
real reconstruction plumbing without claiming completed jittered AA.

## 14. Functional mono resolve, 2026-09-24

The mono adapter follows current-frame writes and the qualified tone/copy
prefix, retains resource identities, and reads the measured camera rows from
CPU uploads. It names the engine-motion source during supported scene draws. At
the original output copy it checks actual bindings once, then resolves the
selected colour/depth with shared engine-record arithmetic and DLSS/FSR
wrappers. Game SS supplies the input extent; the swapchain supplies output
extent. The flat profile continues suppressing the stereo pipeline.

The first functional build supplies zero jitter. A refused frame uses the
original game copy and invalidates history. D3D11.1 context-state isolation and
hook-observation suppression preserve game bindings. Only the qualified copy's
PS0 is replaced; later UI draws stay outside temporal history. Resize hooks
release held backbuffer references before forwarding. EDHM/ReShade chaining and
installation layout are unchanged; their rendered compatibility still requires
game qualification.

Shaders compile during the build. The new WARP rig exercises camera and exact
engine motion, depth, reset, rejected-history composition, TAA scaling, sRGB
view/byte preservation, backend refusal and complete state restoration. It
caught and fixed an HLSL logical expression overwriting an exact engine result
through an out parameter; explicit branching now preserves that result. FSR's
shared wrapper gains an optional infinite-depth context flag, defaulting off
for existing VR callers. Its rig covers finite/infinite context transitions.

Sean selected DLSS at 0.75x SS for the first functional Epic flight. This is a
plumbing qualification, not yet the final jittered AA quality test. DLSS uses
the existing default preset K. Other temporal modes and VR regression remain to
be qualified in game.

## 15. First functional flight: interrupted history, 2026-09-24

Epic `edvr_gfx_20260924_080753.log` matches clean `27217a7d`, build `6AB52DDC`.
Sean reports no visible DLSS engagement and continued shimmer and aliasing. The
runtime reaches 6,705 treated copies. Between 08:09:29 and 08:10:59, counters
increase by 6,486 treated and 1,411 refused (82.1% / 17.9%). The sampled
refusal is `conflicting-hdr-target-or-camera`. Earlier `no-known-tone-pass`
intervals are excluded from that calculation; their screen content is not
established by the log.

DLSS feature creation succeeds for 1920x1080 -> 2560x1440, then 2880x1620 ->
3840x2160 at 08:10:11, then back at 08:10:51, all 0.75x SS and preset K. Source
motion views are given after one warm-up miss per recreated source, with no
producer invalidations. Ruled out: DLSS never engaging, because runtime
acceptance occurs only after `dlaaEvaluate` returns success, which follows a
successful NGX evaluation. Acceptance does not prove useful accumulated pixels:
a reset's final composite deliberately displays current spatial colour, and
this build still supplies zero jitter.

Every refused copy invalidates history. The current aggregate reason cannot
distinguish HDR viewport/depth changes, conflicting camera words/provenance,
explicit resource writes, or disagreement between HDR and tone cameras. Do not
loosen those checks without a witness. A bounded first-cause record for the HDR
actually selected by tone will name the draw, resource identities, viewport and
exact differing camera words. Unrelated HDR targets must not consume this
evidence budget. Cumulative refusal counts and uninterrupted treated streaks
will quantify the effect.

A separate adapter count names missing prior history, frame gaps, changed
colour/depth identities and changed extents. A separate renderer counter will
distinguish requested resets from internal reinitialization, resource/format
changes, frame gaps and camera cuts. This also tests whether hardware interface
identity causes repeated initialization; that remains a hypothesis, not a
rendering fix. No GPU readback or changed acceptance rule is required for this
instrument. Stale capture-only log messages are corrected to identify the
passive observer and direct readers to the actual flat runtime counters.

## 16. Scene camera versus unused handoff binding, 2026-09-24

Epic `edvr_gfx_20260924_091127.log` matches clean `e24b1201`, build `6AB53251`.
The final renderer sample reports 9,921 accepted evaluations: 359 resets and
9,562 continuations, with a longest run of 73 continuations. Ruled out:
repeated renderer/context reconstruction, because init=1, context-change=0,
allocations=1 and full-reset=0. Ruled out: texture identity or extent churn as
the reset source, because adapter depth/color/extent changes remain zero.
Backend failures are zero; one camera-cut reset occurs near the end and does
not explain the recurring refusal bursts.

Four steady-flight witnesses (frames 62688, 63521, 64425 and 65329) identify
`selector-hdr-vs-tone-camera`. The HDR writer is VS 68DDDEF04D9894AF / PS
06332CA168B6DA63, sharing the selected 1920x1080 depth and full XY viewport
with depth range zero. Tone VS F9CFC798F21E9AEA / PS FEE777E92850B390 has the
same VS b1 buffer identity, but a later current-frame upload contains different
rows 270-272, 274 and 275 (word mask 770BBB). This is actual different camera
data, not float noise or an unused single-word mismatch. Refusal windows
contain 60-72 such copies per five seconds, with 10-12 following history
resets. The isolated missing-depth witness during earlier loading remains a
legitimate refusal and is not covered by this fix.

Exact bytecode proves the ownership error: tone VS has no constant-buffer
declaration; tone PS declares only PS b2. The original copy VS 20F383BBAC05C031
has no constant buffer and forwards position/UV; copy PS DED8796049C7BB4A has
only t0/s0 and samples the source directly. In contrast, HDR VS
68DDDEF04D9894AF instructions 77-80 consume VS b1[270..273]. The already saved
copy/tone/HDR vertex bytecodes and copy pixel bytecode match their repository
FNV64 identities. These are offline reads of existing shader captures; the game
test remains Epic only.

The collector correctly records which buffer is bound, but binding alone does
not make its camera rows inputs to a shader. The selector must take its camera
from current-frame HDR scene draws and require agreement with the supported
engine-motion source. Tone/copy prove colour lineage, dimensions and ordering;
their unused b1 binding must neither supply nor reject the scene camera. Keep
strict refusal for genuine HDR/source camera conflicts, stale/missing scene
provenance, depth changes, ambiguous sources and broken colour lineage. No
floating-point tolerance or visual compensation is involved. Engine motion uses
its owned current/previous scene CB copies taken during the source draws, so a
later upload into the game's live b1 does not rewrite those snapshots.

Jitter readiness review: the next implementation needs private,
shader-qualified CB bindings, restored after each draw/dispatch, with engine
snapshots taken before substitution. `flat_projection_math.h` covers the five
measured forward and inverse layouts; active lighting also needs CS b0[10..12]
ray correction under `flat_lighting_contract.h`. Extend the existing dispatch
scopes in `exposure_fix.cpp`, not a second hook chain. Cache private buffers by
observed write provenance; the runtime currently retains only 96 camera bytes,
so full qualified CB contents need bounded shadows. Pass the same pixel phase
to the backend while keeping reconstruction camera rows raw. Offline tests can
verify substitution, restoration and signs. Rendered inverse/depth consistency,
the R32 view-Z writer, mod ordering and a de-jittered current-frame fallback
after a late backend failure still require qualification. Do not enable jitter
merely because the camera-authority correction passes.

## 17. Stable scene admission, 2026-09-24

Epic `edvr_gfx_20260924_093108.log` matches clean `e99c0010`, version
v0.17.0-498-ge99c0010, build `6AB54179`. The final sample reports 6,169
successful evaluations, 6,164 history continuations and five resets. The
longest consecutive treated sequence is 6,144 frames; the longest renderer
continuation sequence is 1,797. Two resets follow requested/lost history; three
are the renderer's existing camera-cut detection. Backend failures, frame gaps,
invalid previous cameras and format changes remain zero. Init=1, allocations=1,
context-change=0 and full-reset=0 confirm stable renderer resources.

Ruled out: recurring scene-camera rejection after the authority correction,
because the sustained flight intervals admit 449-450 frames per five seconds
without increasing the refusal total. No `selector-hdr-vs-tone-camera` witness
appears. Earlier missing-depth refusals and the final no-known-tone transition
remain explicit; this does not establish support for those routes. The three
camera-cut resets are separate from the fixed refusal bursts and do not justify
changing their threshold without further evidence.

The NVIDIA backend is evaluating successfully, with game SS still owning render
scale. The feature-creation line identifies native 2560x1440 DLAA, preset K, on
an RTX 5090; no scaled DLSS feature is created in this run. Read-only
inspection of the saved graphics profiles shows SSAAMultiplier=1.0. Do not
describe this flight as 0.75x qualification just because the requested EDVR
mode is named dlss. Record the next capture's actual input/output extents and
set game SS to 0.75x for that test. The driver and DLSS DLL version are not
reported by this build; flat has no headset/runtime dependency. Projection
jitter is zero, so this run qualifies continuous reconstruction plumbing rather
than final anti-aliasing quality. The next work is the actual rendered phase
contract and its safe output path, not another zero-jitter flight.

Sean reports smearing around the nearby star's corona during camera turns in
this run. This is a distinct image-quality failure; stable DLSS evaluation does
not certify the motion/depth semantics of translucent celestial effects. Do not
attribute it to missing jitter or alter sharpening/brightness clamps. Compare
the exact active corona family and its source depth with the VR corona
motion/rejection path before choosing a correction.

## 18. Jitter inputs and focused depth provenance, 2026-09-24

The mono frame now carries actual current and previous raster phases in input
pixels, positive right/down; camera and engine scene rows remain unjittered.
The prep shader subtracts the current phase before raw-camera/engine
reprojection. SDK motion excludes both phases, and the same current phase is
passed separately to DLSS/FSR. TAA and rejected-pixel output sample the current
raster at the output coordinate plus its phase; previous depth lookup includes
the previous phase. The WARP rig verifies both camera and exact engine motion,
backend phase delivery, TAA depth lookup and zero-phase compatibility.

A standalone spatial output path cancels the input raster phase and invalidates
history after a backend refusal. Its WARP tests include a backend that clears
all context state before failing. This is not yet the runtime's guarantee
against late failure: future nonzero projection integration must prepare
resources before rasterization and invoke the fallback at the qualified
handoff. Allocation/device failure cannot be repaired by the fallback itself.
The live caller continues to supply zero jitter.

The existing two manually armed F10 samples now trace bounded R32 output
candidates before the lighting consumer. Probe admission is limited to the
actual backbuffer extent or exactly 0.75x in both axes; unrelated sizes cannot
consume writer slots. This restriction is diagnostic, not the runtime's
render-scale contract. The capture inspects all eight MRT and UAV slots,
records shader hashes, source views and DSV, and tracks observed copy, clear
and CPU update events. It retains resource identities and never coalesces
shader-write snapshots across commands. There are 96 event slots and
independent 4,096 draw/dispatch query limits; drops and unknown work are
explicit. Selection joins lighting t3 to the same frame's scene depth/HDR.
Bound-output candidates are not automatically proved shader writes: the saved
bytecode must establish actual output consumption.

Writer VS/PS/CS b0..b2 carry frozen CPU data where available. Sample one learns
the selected candidate hashes; sample two can queue up to six nonblocking full
CB readbacks for missing/prefix-only data. Payloads use heap storage and the
sample reset also uses a heap temporary. A flat-only creation cache retains at
most 2,048 shader entries and 16 MiB, then writes only matched candidate hashes
through the existing exact-byte dump helper. Cache/readback misses are explicit
missing evidence. Draw capture now precedes the temporal runtime scope so
actual getters see game bindings before EDVR's motion/output substitutions.

The star report has two separate candidate paths. Earlier Epic frame 36865
records the stock glare train as VS 94D5C556DFD6D705 / PS 912477AEF6958379. It
projects anchors with clip W=1 and samples R32 positive view-Z. Flat has no
dedicated glare coverage/motion or post-DLSS star-glow history treatment. Do
not confuse it with the VR `corona motion` log: that implementation names
thruster-smoke VS 5E417E9DF2E7F9E6 / PS BD801F2FB02522EB. The new flight must
identify the active star draw and depth path; pixel-level motion, coverage and
raw-versus-trained output remain necessary if those do not isolate the smear.
No star rendering fix or brightness threshold change is included here.

The final source build passes in `build/flat-jitter-contract-qualified.log`: 81
parallel jobs plus three quiet jobs, the 254-key config contract and both
installer payload checks. The new phase-aware WARP resolver and capture model
tests pass. Earlier full passes preceded the final capture labels and extent
filter; use the qualified log for the committed source validation.

## 19. Main integration, 2026-09-24

Merge main `28ee5f73` into `codex/flat-temporal-aa` after the clean `a5f979c1`
build, as Sean requested. Keep the experimental flat implementation on this
branch. Main retires UI separation and several old probes, adds pose-reader and
transition-flash diagnostics, and updates build scheduling. Resolve the vscreen
conflicts by retaining flat clear/copy capture and original-binding draw
ordering while taking main's retired-path removals. The new diagnostics read
profile-gated configuration, so flat cannot enable their VR hooks.

The next Epic flight remains a focused F10 capture near the star while turning
the camera, with game SS set to 0.75x. Projection jitter is still disabled and
the star-corona smear is not yet fixed. Preserve the live INI during
deployment.

The merged source passes `build/flat-main-merge-qualified.log`: 79 parallel
jobs plus three quiet jobs, the 255-key config contract, flat mono WARP and
capture policy tests, and both installer payload checks.

## 20. Stable scaled DLSS and depth-writer capture, 2026-09-24

Epic `edvr_gfx_20260924_102404.log` matches clean `10cb20f0`, version
v0.17.0-554-g10cb20f0, build `6AB54DBF`. NVIDIA creates scaled DLSS with
1920x1080 input and 2560x1440 output, quality mode and preset K, on RTX 5090.
This is the requested game SS 0.75x run; unlike section 17, it is not native
DLAA. Projection jitter remains zero. Driver and DLSS runtime versions remain
unreported; this mono run has no headset or VR runtime dependency.

The last renderer report has 4,951 successful evaluations, 4,949 history
continuations and two requested/lost-history resets. Camera cuts, frame gaps,
invalid previous cameras, format changes and backend failures are zero. Init=1,
allocations=1, context-change=0 and full-reset=0. The longest treated streak is
4,922 frames and the longest continuation run is 4,921. The final no-known-tone
transition adds 269 refusals after sustained flight. Sean reports no visible
corona smearing this time. Record that as not reproduced; no star rendering fix
was applied and the conditions differ from the prior native run.

F10 samples at frames 58340 and 58789 each identify one R32_FLOAT RTV writer
before both active lighting consumers. Both have zero writer, draw-query,
dispatch-query and fallback drops, and zero unknown command lists. The matched
writer uses VS DEF19B035D5EDEDC / PS CB95394B50D737D6 at 1920x1080. All four
required producer/glare shader saves succeed despite one unrelated cache drop.
Sample two completes all three queued CB readbacks (VS b1 5376 bytes, VS b2 48
bytes, PS b2 16 bytes). The active stock glare shader pair remains
94D5C556DFD6D705 / 912477AEF6958379.

The saved writer VS (332 bytes) passes POSITION and TEXCOORD0 unchanged; it
does not consume the bound VS constants. Its PS (384 bytes) samples PS t0,
computes `min(cb2[0].z / (sample + cb2[0].w), 1e17)`, and writes target0.x. In
both samples, t0 is the selected scene depth (resource ending E7E0, view format
21), and the output is the lighting consumers' R32 view-Z texture (resource
ending DCE0, format 41). Sample two's completed PS b2 readback is `(0.025, -0,
0.025, -0)`, establishing positive view-Z = 0.025 / reversed Z. Sample one's PS
constant was unavailable; do not claim it was independently measured twice. The
unrelated bound VS b1/b2 need no substitution for this shader pair. This proves
the captured source, output and shader transform, not actual pixel
correspondence under a nonzero raster phase.

Ruled out: an additional projection-matrix consumer inside this captured R32
conversion pass, because neither saved shader reads a projection matrix. Retain
its original UV mapping and depth coefficients when implementing jitter
upstream. Private scene and lighting bindings, raw engine snapshots, resource
preflight and post-rasterization fallback remain implementation work; rendered
alignment under nonzero jitter remains a qualification requirement.

This flight review changes documentation only. Epic remains on `10cb20f0`;
subsequent documentation commits do not require another build or flight.

## 21. Private projection binding foundation, 2026-09-24

`flat_projection_bindings.h` adds a bounded full-width constant-buffer shadow
bank (default 64 identities, 64 KiB each, one 4 MiB payload allocation). A
resource lifetime token, write generation and bank epoch distinguish reusable
COM addresses and replaced data. Map start, incomplete writes and unknown
mutations invalidate old bytes. Only a complete observed write can publish a
new snapshot. Registration, lookup and writes allocate nothing after bank
construction. Callers retain source resources and supply new lifetime tokens
after release; these are owner-thread APIs.

Pure patch preparation validates up to eight nonoverlapping spans before
changing any destination bytes. It supports the five measured projection
layouts plus the lighting UV-ray layout with its explicit tile/phase contract.
It clones full buffers, preserves unrelated bytes, and never changes the raw
camera/engine snapshot. A shader hash is not admission by itself: the caller
still must establish the exact draw, camera ownership and full write history.

`flat_projection_scope` preflights private dynamic buffers and caches uploads
by current source provenance, phase and complete patch request. It always looks
up fresh bank contents; callers cannot reuse a stale snapshot after
invalidation. Invalid preparation hides the old replacement. A reusable binding
plan performs immutable device, descriptor and D3D11.1 capability checks once.
The scope checks all actual VS/PS/CS buffers and first/count ranges before
changing any binding, then restores exact originals on exit. Its internal calls
suppress EDVR observers without suppressing the enclosed game work. No
allocation, feature query or descriptor query occurs in the draw scope.
Prepared-resource tokens invalidate retained plans after failed preparation or
owner destruction. A new upload requires explicit token refresh, which does not
repeat static resource checks. Callers must prepare from the current bank
before executing a plan; tokens do not replace source-write observation.

CPU tests cover full 64 KiB snapshots, stale/partial/mapped data, identity
reuse, all patch layouts, untouched raw bytes and late atomic refusal. WARP
uses a real CS with a nonzero CB range to verify the private patch reaches the
GPU and the original is read after restoration. It checks all three stages,
restoration after ClearState, stale second-slot refusal without partial
binding, duplicate slots, deferred-context refusal, nested observer guards,
cache reuse and upload invalidation. Tests also reject retained plans after
failed preparation, changed upload revision and owner destruction. Zero-phase
lighting accepts either sign of floating-point zero and preserves the full
original buffer.

This foundation is not wired into game draws or dispatches yet, and adds no
per-draw work to the active zero-jitter renderer. Existing base setters do not
observe Context1 range binds, PS observation is capture-only, and CS has no
general CB bind observer. Runtime integration must either observe those ranges
and full write lifetimes or explicitly refuse them, then supply the measured
shader/owner admission table. Resource preflight and the already tested spatial
fallback must also be joined to the actual handoff before any nonzero phase.
The R32 depth conversion stays unchanged; star glare is a separate qualified
consumer. No Epic reinstall or repeat zero-jitter flight is warranted for this
offline layer. Keep installed build `10cb20f0` for interpreting existing logs.

Final source validation: `build/flat-private-bindings-final.log` passes all 79
parallel jobs and three quiet jobs, the 255-key config contract and both
installer payload checks. The final run includes the prepared-token lifetime
tests and signed-zero lighting test; earlier passes preceded those additions.

## 22. Runtime preparation and resolver recovery, 2026-09-24

F10 arms a 900-frame owner-thread preparation audit. It uses the captured exact
shader recipes, verifies actual VS/PS/CS bindings and D3D11.1 constant ranges,
and prepares private buffers at a proposed pixel phase of (0.25, -0.25). It
never binds those plans: game rasterization and temporal backend inputs remain
at zero phase. Normal operation outside the audit does not maintain the new 4
MiB full-buffer bank or query candidate descriptors. Existing camera and
engine-motion snapshots are taken from the original bytes before preparation.

The runtime observes initial data, full UpdateSubresource writes and mapped
writes before Unmap. Partial writes, copies and unknown work invalidate shadow
provenance. It retains bounded source identities, caches at most 32 plans and
rejects unsupported ranges. Lighting preparation checks the actual integer
extent/grid metadata and 120-pixel tile contract. The R32 view-Z conversion has
no projection recipe; the measured glare and embedded HUD recipes remain
separate consumers. The private binding record is named
`FlatPrivateProjectionBinding` to avoid the existing diagnostic model's type.

Static CBs created before F10 can be missing from the CPU observer, as the
previous flight demonstrated. An opt-in cold path admits only an explicitly
requested CB, copies its full contents to staging and ends an event query.
There are at most eight pending copies and 16 attempts per arm. Owner Present
polls without Flush or blocking Map; a changed lifetime/write token discards
the result. A 120-poll timeout bounds retained work. This is diagnostic
preparation, not a production per-frame GPU readback requirement.

The qualified copy handoff records actual texture and SRV metadata for the next
frame's resolver preflight. Preflight rejects invalid extents, modes, formats,
mip/range layouts and sample counts before allocating. It prepares
renderer/spatial-output resources and checks backend availability. A
size-specific DLSS/FSR feature still requires live textures and can fail later;
the log exposes that deferred step. A late temporal resolve failure now tries
the tested spatial resolve, restores the copy binding after the draw and
invalidates temporal history. Spatial recovery never counts as temporal
success. Earlier source/handoff refusals still need a complete recovery policy
before live jitter can be enabled.

Readiness logs distinguish preparation failures, unknown scene draws, depth
association, cold-copy outcomes and resolver/fallback/backend availability.
Per-shader outcome counts retain successful preparation and each refusal code
in a bounded 256-entry table, with explicit overflow observation counts. This
avoids mistaking an aggregate success count for coverage of a particular
consumer. Unsuccessful resolver preflight retries at most once per second for
unchanged metadata; a changed plan is checked immediately. Depth association
alone does not prove camera ownership or complete HDR and material coverage.
Every summary explicitly reports `raster-authorized=0`. The next Epic flight
must establish which candidate buffers can be prepared from observed writes or
stable cold snapshots, which recipes/ranges refuse, and which scene consumers
remain unknown. Existing F10 shader captures and these new counters provide
those discriminating signatures in one flight.

Focused CPU/WARP tests cover recipe selection, actual private-buffer contents,
stale/ranged bindings, cold-path opt-in, stable completion and
intervening-write discard. Resolver tests exercise metadata-only preflight and
a backend failure that uses its preallocated spatial output without further
texture allocation.

Final source validation: `build/flat-readiness-final.log` passes all 79 pooled
jobs and three quiet jobs, both flat test rigs, the 255-key config contract and
both installer payload checks. This includes per-shader outcome reporting,
preflight retry cadence and filtering non-CBs from the creation observer.

## 23. Readiness flight and compute audit correction, 2026-09-24

Epic `edvr_gfx_20260924_112608.log` verifies `v0.17.0-557-gf1ea02fe`, build
`6AB55BF9`, linked 17:20:57 UTC. The run begins with native-size DLAA, then
creates DLSS at 11:28:04.750 for 1920x1080 input and 2560x1440 output, quality
mode, preset K. The F10 audit starts at 11:28:17.092 and completes all 900
frames at 11:28:27.185. Its scene samples confirm the scaled dimensions.

The final runtime summary reports 6,092 temporal calls, 6,084 history
continuations and eight resets: three requested/lost-history resets and five
camera-cut detections. No backend failures, invalid previous cameras, frame
gaps or format changes occur. The longest adapter-treated streak is 4,333; the
longest renderer history-continuation streak is 1,363. Earlier startup refusals
include conflicting HDR/camera and missing tone passes; during the F10 interval
the refusal count stays at 7,439, then 51 missing-tone refusals appear at the
end. Do not describe the whole session as refusal-free or camera-cut-free. No
visual symptom report was provided with "flew it".

The completed preparation audit counts 362,165 draws, 19,477 dispatches, 47,107
candidates and 47,107 successful preparations, with zero refusals. All 21
recognized draw tuples prepare, including 900 stock-glare draws. It records 900
depth-unassociated candidates, 81,566 unknown draw observations and 58 distinct
outcomes without overflow. There are 188,507 observed full writes and no
cold-readback attempts: this flight does not exercise that fallback. Renderer,
spatial-output and backend-availability preflight all pass; size-specific
backend feature creation remains deferred. There are no spatial fallback
attempts. Actual raster and backend phases remain zero.

The separate compute capture proves both lighting shaders `5998146D464F5C0E`
and `EB0245DE0BB23BB6` ran in samples 1 and 2, with matching 1920x1080 R32
inputs and selected scene depth/HDR. Yet the readiness outcome table contains
no CS entries. Source trace confirms the cause: `hookedCSSetShader` calls
pointer-only `bindingSet`, while the audit read `bindingShaderHash(Cs)`, whose
hash was never populated. The correction resolves the registered hash from
`bindingGet(Cs)` inside the audit only, then retains actual-CS verification in
`qualifyProjection`. It adds no lookup to normal unaudited dispatches.

Ruled out: missing CPU writes prevented the recognized draw preparations,
because every one of their 47,107 attempts succeeded. Ruled out: absent
lighting dispatches explain the missing readiness CS entries, because both
exact shader hashes appear in both independent compute samples. Lighting
preparation readiness itself remains unmeasured; the previous aggregate success
cannot establish it.

Offline exact-hash bytecode classification divides the 37 unknown tuples:

| Classification | Pairs | Observations |
| --- | ---: | ---: |
| VS b1 rows 270-273 projection | 22 | 50,732 |
| VS b0 rows 4-7 projection | 5 | 20,934 |
| VS b2 rows 10-13 projection | 3 | 2,700 |
| VS b2 rows 6-9 projection | 2 | 1,800 |
| VS b2 rows 7-10 projection | 1 | 900 |
| Pass-through/zero or inert full-screen pass | 3 | 2,700 |
| Bytecode unavailable | 1 | 1,800 |

The largest missing projection recipes are `BFE51414CC3024B4 /
DB79AE788E049DFD` (11,700), `81216C77F90DEDD6 / A2965EC2931A39C8` (9,910),
`B7790CBFC6554097 / 8DEF46452FA459F5` (8,324) and `7B0DC42D383F694C /
0DF03E64DF9DBEF1` (6,300). B779's PS samples depth using interpolated clip
coordinates, requiring aligned upstream projection. `EB5234DB6ADB491D /
B7D50283329322C3` and `DE545DC8EE4FBB87 / 91F8937EDA723663` are additional
material companions of known projection-bearing VS hashes. Projection use is
not proof of camera or HUD/radar ownership and does not authorize jitter.

The three unchanged tuples are `FC1193AFFC596F74 / 258B95AC99520C1F`,
`E8FDC0D92EEBA6D7 / 258B95AC99520C1F` (position pass-through, zero output), and
`53211E8C072CD02E / B403F48CB35D9739` (full-screen triangle, inert PS). The
unclassified pair is `5EAFFCD01B97D0C4 / DD371C57C9093BB8`; neither shader
exists in the saved ASM or Epic DXBC corpus. Each future F10 arm now requests
those exact creation bytes once using the existing bounded cache and logs
success or explicit missing evidence. This does not add a shader recipe. Local
evidence is under `build/flat-audit/coverage-classified.json` and
`build/flat-audit-current/missing-shader-closure.md`, with exact-hash ASM.

Keep the installed Epic build at `f1ea02fe` while qualifying these gaps. A
repeat of the same installed flight would not measure the corrected CS audit.

Validation: `build/flat-flight-audit-final.log` compiles the CS lookup and
one-shot shader requests, and passes all 79 pooled plus three quiet jobs, both
flat rigs, the 255-key config contract and installer payload checks. No new
Epic installation or flight is claimed for these corrections.

## 24. Expanded recipes and scene reference diagnostics, 2026-09-24

The 33 projection-bearing VS/PS pairs identified in section 23 now have exact
recipes. Their 30 distinct VS bytecodes confirm both first row and
multiplication convention: 22 pairs use b1[270..273] scalar-weighted rows, five
use b0[4..7] dp4 rows, three use b2[10..13] dp4 rows, and three use b2[6..9] or
[7..10] scalar-weighted rows. Additional materials require the captured PS
hash; this does not broaden engine-motion families or scene-source selection.
Every new pair and adjacent PS rejection is tested. The missing-bytecode pair
remains unknown, while the three inert pairs have a separate
`bytecode-unchanged` outcome after checking actual shader bindings.

The preparation audit adds a separate numeric comparison against the first
named raw scene-camera snapshot of the current frame. A bounded owner-thread
copy API reads only complete valid shadow data and leaves its destination
untouched on invalidation, missing data or an out-of-range request. It exposes
no retained pointer into the shadow bank. Tests cover invalidation, mapped
transactions and completion as well as bounds. All new work is confined to the
F10 interval; no private plan is bound and live jitter stays zero.

Each prepared tuple reports one reference classification:

- `canonical`: the same VS b1 resource and exact 96 camera bytes, with current
  reference association and valid scene-camera encoding.
- `basis-match`: a forward dp4 matrix has exactly the scene transpose's spatial
  coefficients and depth/near row. Translation residual is measured separately
  and never thresholded into an ownership claim.
- `unmatched`: finite supported evidence does not meet the exact identity or
  basis relation. An equal basis on a different b1 identity is not canonical.
- `unavailable`: missing, stale, short, non-finite or degenerate evidence, or
  no uncontested depth-associated scene reference yet in the frame.
- `unsupported`: inverse/lighting or embedded local-matrix relationships not
  implemented by this diagnostic. They are not silently counted as matches.

The pure classifier uses captured matrices, with tests for changed pose,
different resource identity, unequal near plane, a normalized-axis false
positive and an embedded-HUD negative case. Forward translation residuals are
reported in double precision without introducing an empirical acceptance
tolerance. A matching basis with different translation deliberately remains
only a basis match. Residual sample counts distinguish absent measurement from
an observed zero. These counts are attached to the existing bounded per-tuple
outcome table, with explicit overflow.

The first full build caught a rank-check failure: cofactor cancellation left a
tiny nonzero determinant for duplicated captured basis rows. The corrected
validity check certifies nonzero rank only outside a derived double-precision
rounding-error bound. This is arithmetic uncertainty, not a coefficient-match
tolerance. Tests cover every duplicated/proportional row order, dependent
nonproportional rows and tiny/large nonsingular bases with either determinant
sign. Uncertain rank reports unavailable evidence.

This checks the primary forward rows in a prepared constant buffer, not every
consumer of that buffer. In particular, the deferred two-patch recipe retains
the stored forward-matrix comparison although its VS consumes the inverse-ray
rows. Actual shader and CB/range getters are verified before preparation; depth
association still comes from the binding shadow. Therefore no numeric label
proves actual DSV/HDR ownership, local-camera equivalence, complete inverse
alignment or whole-frame closure. Do not enable raster jitter from these
counters alone.

The next combined capture also includes the corrected CS hash lookup and
one-time requests for `5EAFFCD01B97D0C4` and `DD371C57C9093BB8`. It should
distinguish remaining buffer/range failures, unmatched reference families,
explicitly unsupported local/inverse relations and genuinely unknown shaders in
one flight. EDHM/ReShade chaining and installer scope are unchanged.

Final source validation: `build/flat-expanded-projection-retry.log` passes all
79 pooled and three quiet jobs, both flat rigs, the 255-key config contract and
both installer payload checks. This includes the rank regression fix. An
earlier retry stopped in the existing sleep/order-based `run_jobs` self-test;
it passed in isolation and in this final full run without changes to the
scheduler.

## 25. Expanded preparation flight, 2026-09-24

Reviewed Epic `edvr_gfx_20260924_130742.log` with `tools/edvr_log.py
--expect-build ad8b586a`: version `v0.17.0-559-gad8b586a`, build `6AB56497`,
linked 17:57:43 UTC. This is the installed expanded-recipe build. The user
reported running it; no visual quality or corona-smear result was supplied.
Environment: mono D3D11, RTX 5090, DLSS quality preset K, 1920x1080 input to
2560x1440 output (game SS 0.75 per axis). Driver and DLSS runtime versions are
not recorded by this build. Headset/runtime are N/A. The feature creation at
13:08:19.556 establishes that DLSS engaged; it does not qualify antialiasing
with raster jitter still zero.

The F10 audit completed 900 frames at 13:09:35.844: 337,048 draws, 19,795
dispatches, 102,865 candidates, 102,864 prepared and one refused. There were 57
outcome tuples with no overflow, 2,700 explicitly unchanged draws and zero
unknown scene draws in this interval. Actual shader/CB checks and private
preparation passed for the prepared candidates. Plans remained unbound.
Resolve, fallback and backend preflight were ready; backend feature creation is
intentionally deferred in that separate preflight result.

Both lighting compute shaders are now observed by the preparation audit:
`5998146D464F5C0E` prepared 959 times and refused once with
`missing-full-write`; `EB0245DE0BB23BB6` prepared 960 times. The cold path
queued one snapshot and rejected it as stale, with zero completions, failures,
pending copies or timeouts. This demonstrates the mutation guard, not a
successful cold-readback admission. It does not justify skipping full-write
provenance or relaxing that guard. No spatial fallback ran in this flight.

The 54 prepared tuples partition into the following primary-recipe reference
observations (sum 102,864):

| Classification | Observations | Meaning |
| --- | ---: | --- |
| canonical | 48,109 | Same current scene b1 identity and camera bytes |
| basis-match | 33,936 | Exact spatial/depth relation; translation separate |
| unmatched | 1,800 | Two finite supported families differ from reference |
| unavailable | 12,600 | Current comparison prerequisites not established |
| unsupported | 6,419 | Local/inverse/lighting relation not implemented |

The two unmatched pairs each occurred 900 times:
`4D516EF05C68FFA5/147E748F4CD3AE9A` has maximum spatial/depth error 0.391068339
and translation residual 715292421; `5E417E9DF2E7F9E6/BD801F2FB02522EB` has
3.46919596 and 546229445. These are not evidence of a rendering defect by
themselves: local transforms can change the relationship. They refute admitting
every prepared dp4 matrix as the selected scene camera without further proof.

Among exact basis matches, `CFCA8FFC6B058630`, `88DCF1164C640EC3`,
`81216C77F90DEDD6` and `2CECEC3065EF0D4A` reach translation residual
2438.38834; the other observed matching families reach 0.000470820162. Do not
turn either magnitude into an empirical ownership threshold. The unavailable
rows are `0EE43D81E394E70C` (900) and `BFE51414CC3024B4/DB79AE788E049DFD`
(11,700); the aggregate label does not distinguish every missing prerequisite.
There are 12,720 depth-unassociated candidates overall, which is a separate
population from unavailable supported comparisons. Actual DSV/HDR association
and complete inverse/lighting alignment remain outside this diagnostic's proof.

The requests for `5EAFFCD01B97D0C4` and `DD371C57C9093BB8` both report missing
creation bytecode at 13:09:25.846. Neither appears among completed audit
outcomes. Zero unknown draws in this flight therefore does not close the
missing pair from the previous flight, nor establish general scene coverage.

The final renderer report records 3,768 accepted calls: three history resets
and 3,765 continuations, one camera cut, zero backend failures and no
frame-gap, invalid-previous-camera or format-change resets. Its longest
continuation run is 2,089. The adapter's longest treated streak is 3,734, with
no added refusals through the steady interval from 13:09:23 to 13:10:03.
Earlier reports include 604 conflicting-HDR/camera refusals and many
no-known-tone-pass refusals; 327 additional no-known-tone-pass refusals appear
at the end. The log alone does not identify the user's screen during those
intervals. Safe recovery after early refusal remains required before jitter can
be enabled.

Ruled out: the corrected compute audit still being blind, because both exact
lighting hashes now have preparation outcomes. Ruled out: private recipe
preparation alone proving scene-camera equivalence, because 1,800 supported
comparisons are unmatched and substantial unavailable/unsupported populations
remain. Missing-shader closure and successful cold-snapshot admission remain
untested rather than passed. No repeat flight is needed to read the existing
shader evidence; the next change must address the remaining contracts before
another combined test is requested.

Offline bytecode review narrows that next contract. Both unmatched VS families
consume b0[4..7] for SV_Position. `4D516...` also exports clip xyw to its PS,
which uses it for depth UV and discard (`build/flat-audit-current` exact-hash
VS/PS disassemblies). `5E417...` scales its vertex using cb2, exports clip xyw
from b0[4,5,7], adjusts output clip z by +15.01, and its PS likewise samples
depth and discards (`build/flat-audit` exact-hash disassemblies). Neither is
safe to exclude merely because its raw matrix differs from the named scene
camera. Existing `build/flat-projection-flight/b0ownership.txt` shows a
different rotated/translated relation for `5E417...` across two earlier frames;
it does not establish a stable world-model transform. The earlier
`conclusion.md` establishes a camera-relative relation for CFCA and selected
inverse paths only, not every newly observed family.

The next bounded diagnostic should collect both pairs' b0[4..7], relevant
local/model and scene-camera constants, and actual PS depth-SRV/DSV/HDR target
identities together in a selected frame. Factor the projection using the
bytecode's real vertex scaling and coordinate convention, then verify clip UV
and depth reconstruction together. Residual maxima alone cannot distinguish
local coordinates from a different projection. This flight therefore calls for
a focused contract/capture change, not a threshold adjustment or another run of
the unchanged build. No rendering code or installed files were changed during
this review.

## 26. Reuse VR contracts; inspect flat bindings, 2026-09-24

Sean correctly challenged repeating VR's solved motion/jitter work. Section
25's raw camera mismatch is not a prerequisite for homogeneous projection
jitter. Adding jx times clip W to clip X (and jy times W to Y) shifts a local
or model-composed projection just as it shifts the scene projection. The
existing `flatJitterForwardDp4` already does this; its recipe admission did not
require camera equality. The diagnostic must not become that extra gate. An
offline regression composes rotation, nonuniform scale and translation with the
captured camera, exercises both branches of the smoke vertex scale, and checks
raster/depth UV displacement plus unchanged clip Z/W and the shader's
post-projection depth bias. This extends the existing algebra tests; it does
not invent another motion estimator or jitter convention.

The exact solar pair is already named in `planet_motion.h`; the other pair is
the thruster-smoke path in `ui_depth.cpp`. Its internal corona-motion naming
must not be confused with the user's star-corona report: the star glare path in
section 18 is `94D5C556DFD6D705/912477AEF6958379`. No causal link between the
two unmatched comparisons and the reported smear has been established. VR's
solar visibility/motion and smoke coverage are prior implementation knowledge
to reuse, not shader behaviour to rediscover by flying again.

The genuine architectural difference is injection and scheduling. Native VR
computes a phase in `native_temporal.cpp` and supplies a shifted frustum
through `openvr_system.cpp`'s projection APIs. Elite then derives its related
matrices from that projection. Flat does not call that VR boundary, so it needs
D3D11 constant-buffer substitution and matching inverse/lighting handling.
Engine rigid-record motion HLSL and DLSS/FSR backend entry points are already
shared. `flat_mono_resolve.cpp` is a separate mono renderer, not the entire VR
`temporal_pass.cpp`; this retains the performance-first boundary requested for
the feature. Flat scene/handoff selection and recovery after an early refusal
still need integration before a live nonzero phase is enabled.

The focused F10 diagnostic samples only the two exact solar/smoke pairs before
their original draw. Each pair gets at most two attempts in distinct frames,
separated by at least 90 frames, during the existing 900-frame window. Actual
shader getters verify the nominated pair. Current full CPU shadows supply
range-aware shader constants; missing/unbound/range-invalid slices are logged
as such. Raw named-camera words and actual PS t0 depth, output/depth views and
viewport are recorded together. A completion report includes zero attempts if
the pair never ran. This is flat target/depth evidence, not a camera-equality
test or a live-jitter authorization. No new GPU staging copy or wait is added,
and the diagnostic does not bind private plans or change backend phase.

The later copy observer correlates each sampled frame with the modeled HDR
selection and depth. It labels that model separately from the actual draw
bindings; the existing actual-copy validation still follows. Full source build
`build/flat-local-binding-capture.log` passed all gates, including the focused
local-transform regression, flat CPU/WARP rigs, config contract and both
installer payloads. The live flight build is stamped from the committed source
before installation. No EDHM/ReShade chaining or INI setting changes are part
of this diagnostic.

## 27. Focused binding flight, 2026-09-24

Verified Epic `edvr_gfx_20260924_161644.log` with `tools/edvr_log.py
--expect-build 25a634b2`: version `v0.17.0-561-g25a634b2`, build `6AB57975`,
linked 19:26:45 UTC. DLSS created successfully at 16:17:16.701, quality preset
K, 1920x1080 input to 2560x1440 output. This is mono D3D11; headset/runtime are
N/A. The user reported the run without additional visual feedback. Raster and
backend jitter remained zero; this is not an AA-quality qualification.

Both exact pairs were sampled in frames 35018 and 35108. Every sample used one
full 1920x1080 viewport, mip zero, single-sample, single-slice textures. The
actual HDR resource `0000023317EE1BE0` (format 26) and DSV resource
`0000023317EE10E0` (typeless 19, view 20) match the modeled selected HDR and
depth in all four copy-handoff records. The named camera reference was current,
with no uncertain-prefix or foreign-context flag. Actual PS t0 was the same
separate R32 depth resource `0000023317EE1660` (typeless 39, view 41) for both
pairs. Its different identity from the hardware DSV is expected for the
separate linear-depth representation; it is not a target mismatch by itself.
Handoff comparison is explicitly against the model, while original draw
bindings were obtained through actual D3D11 getters.

Later captures in this same log, frames 35466 and 35917, identify the R32
writer to `0000023317EE1660` reading `0000023317EE10E0` through PS t0. Its
exact VS/PS pair is the previously established conversion pass: the VS passes
through, and the PS emits view Z from depth using cb2[0].z/(depth+cb2[0].w),
capped at 1e17 (section 20). This connects the sampled texture identities to
the existing conversion contract. The writer was not captured in frames
35018/35108 themselves, so the within-frame lineage is an inference from stable
resources and the known pass, not a new direct observation.

The VS b0[4..11] slices were current complete shadows in every sample, as were
the sampled scene/local b1 values. Only solar PS b2[2..7] and smoke VS
b2[0..1]/PS b2[0..2] lacked shadows. Consequently the broad diagnostic reports
zero complete captures, but all four target captures and handoff links exist.
Those missing material/local-scale slices are left untouched by the existing b0
projection recipe. Homogeneous clip jitter applies after the shader's local
vertex scale, independent of its value; missing those optional bytes does not
invalidate the available projection patch. Do not add readback or request
another flight merely to turn the broad completion counter green.

The 900-frame preparation interval finished at 16:18:53.668:

- 350,263 draws and 20,265 dispatches; 112,590 candidates, 112,589 prepared,
  one `missing-full-write` refusal from CS `5998146D464F5C0E`.
- Lighting CS preparations: 953 for `5998146D464F5C0E`, 954 for
  `EB0245DE0BB23BB6`. One cold snapshot queued and became stale; no cold
  completion, timeout, failure or pending copy remained.
- 60 outcome tuples, no overflow, zero unknown scene draws and 2,700
  bytecode-unchanged draws. The missing 5EAFFC/DD371 pair again had no retained
  bytecode and was not observed; broader coverage remains unqualified.
- Resolve/spatial/backend preflight ready, zero spatial fallbacks. Raw camera
  comparisons partition 112,589 prepared observations into 52,880 canonical,
  36,851 basis-match, 1,800 unmatched, 12,600 unavailable and 8,458
  unsupported. Section 26's local-matrix interpretation still applies to these
  labels.

The final renderer count is 2,767 accepted calls, two resets and 2,765 history
continuations, with zero backend failures, camera cuts, frame gaps,
invalid-previous-camera or format-change resets. The longest continuation is
2,735 and the longest treated adapter streak is 2,736. Refusals did not
increase through the steady interval; earlier reports include 622 conflicting
HDR/camera refusals and no-known-tone-pass refusals, with 46 further
no-known-tone-pass refusals at the end. Screen/menu state during those
intervals is not inferred from the log.

Ruled out: these sampled local projection draws belonging to a different HDR or
hardware depth target, because actual resources match the selected scene in
both sampled frames. Ruled out: a missing projection shadow explaining the
partial-capture label, because the missing slices are the untouched b2
material/scale data, not b0[4..7]. These results close the focused binding
question for this scene. Remaining work is live phase scheduling/binding,
coherent inverse/lighting inputs and recovery on earlier handoff/source
refusal. No rendering code or Epic files changed during this review.

## 28. Live phase and refusal recovery, 2026-09-24

The runtime now uses the existing eight-phase `temporalJitter` sequence after
two accepted, complete zero-phase frames. The existing
`experimental.temporal_aa_jitter=off` setting keeps rendering at zero phase.
The flat shadow bank persists across frames rather than being an F10-only
allocation. F10 reports the active path without destroying its resources.
Engine motion still sees the original game constants; the raster command alone
sees private jittered buffers. The mono renderer receives the current phase and
the previous accepted phase in render pixels. Camera rows and engine snapshots
remain unjittered.

Draw scopes apply the exact forward/inverse recipes immediately before the
original draw and restore all original CB identities and ranges afterward.
Dispatch scopes similarly wrap the two qualified lighting CS variants after the
raw diagnostic observer has run. Their inverse-ray patch uses the actual frame
phase, retaining the established 120-pixel tile/grid checks and shared Halton
bounds. Actual shaders and CB bindings are verified; actual draw DSV and
compute depth inputs associate the command with the current named scene depth
or the preceding accepted frame's retained depth. Local projection matrices are
not required to equal the scene camera.

Warm-up may allocate private resources or request bounded cold snapshots.
During a nonzero frame, preflight can only reuse established buffer/recipe
topology: it cannot allocate a new private buffer/plan or queue a cold copy.
Topology cache entries retarget phase values instead of consuming another of
the 32 plan slots per phase. Exact phase/write validation and prepared-token
revocation still prevent stale plans from binding. WARP regressions cycle 64
ordinary and 64 lighting phases, refresh after source writes, and verify
no-allocation refusal plus old-plan invalidation.

A refusal before the first applied projection makes the remaining frame
zero-phase. A refusal after an application keeps the chosen phase fixed for
remaining known commands, rejects temporal history, and selects single-frame
spatial recovery at the output copy. Early scene/handoff/source refusals can
recover through independently checked actual copy shaders, input texture,
output target and viewport, without inventing a scene camera. If that copy
cannot be verified, the original command is preserved and recovery failure is
reported. Following frames return to zero-phase warm-up. Spatial recovery does
not recreate a missed draw or promise that a partially patched frame has no
transient mismatch; it prevents that frame from entering temporal history.

The pure phase controller tests warm-up, fixed phase, early/late refusal,
recovery, previous accepted phase, disablement and resize. Existing WARP
resolver tests cover nonzero raster phases with raw engine snapshots, backend
failure, spatial displacement and state restoration. Runtime logs distinguish
actual jittered draw/dispatch counts, accepted temporal frames, partial
coverage and spatial fallback. Preparation counts alone remain insufficient to
claim live operation or image quality. Installer components and EDHM/ReShade
forwarding are unchanged.

Validation: the full build and all gates pass in
`build/flat-live-jitter-final.log`. The first attempt exposed a test-only
expectation error: both 64-phase loops include one zero-offset phase, for which
a null binding plan is intentional. The corrected tests require no binding for
that phase and active bindings for the other 63. Their focused WARP rerun and
the full suite pass; production behavior was unchanged by the test correction.
Final review removed a duplicated preflight reset and updated stale audit-only
comments; `build/flat-live-jitter-verified.log` confirms the final source also
passes the full build and all gates before commit. In-game nonzero phase,
visual quality and VR regression remain unqualified.

## 29. Live integration flight refused, 2026-09-24

Epic `edvr_gfx_20260924_165053.log` matches `0150638a`, version
`v0.17.0-563-g0150638a`, build `6AB5A844`, linked 22:46:28 UTC. DLSS was
requested, with 1920x1080 scene resources and 2560x1440 output. This was not a
successful live-jitter test: all reported phases, applied draws/dispatches,
accepted frames and history counters remained zero. The final runtime total was
17,244 refused copies. Backend preflight never ran.

Two independent blockers are evidenced:

- `Config::getString` returns `off` for keys rejected by
  `runtimeProfileAllowsKey`. Flat's allowlist omitted
  `experimental.temporal_aa_jitter`, so the newly wired default-on read was
  always off. This also explains zero jitter-refusal counters despite unknown
  projection recipes in the F10 audit. Fix the existing key's flat allowance
  and exercise the actual Config getter in a regression; preserve explicit off
  and suppression of unrelated VR features.
- Main-scene conflict witnesses at frames 32715, 33613 and 34509 identify VS
  `CFA91824129ECBBC` / PS `DFCBA0EC70B03C9B` writing the selected 1920x1080 HDR
  target with no DSV. The preceding reference draw uses `7E38A6AA1269C901` /
  `7CECABDE34FFBE9E` with matching scene depth. The selector marks this
  `missing-depth-or-dsv` before any backend call. Earlier frame 25275 is a
  different depthless copy pair. Determine the exact main pass's semantics
  before admitting a depthless HDR write; do not globally remove depth checks
  or label this a motion/jitter math failure.

The 900-frame audit observed 276,575 candidates, 261,131 preparations, zero
private-preparation refusals, 12,744 depth-unassociated observations and 20,173
unknown scene draws across eleven pairs. These are zero-phase preparations, not
raster bindings. The previously missing `5EAFFCD01B97D0C4` / `DD371C57C9093BB8`
pair appeared 4,395 times, and F10 saved both creation blobs (1,768 and 5,880
bytes). Its earlier absence is no longer a reason to defer offline shader
analysis.

Ruled out: stale DLL, because the log stamp matches the installed commit. Ruled
out: backend failure caused this flight's refusal, because selection never
reached backend preflight or resolve. The allowlist defect and HDR selection
refusal are separate; fixing only the former cannot make this captured scene
resolve. Scene context and remaining shader coverage still need analysis before
another qualification build is installed.

Offline bytecode resolves the missing-depth pass's role. The exact Steam
`edvr_logs/shaders/vs_CFA91824129ECBBC.dxbc` (332 bytes) forwards position and
UV with two moves, without CBs or resources. Its paired PS (372 bytes) samples
t0, copies RGBA to RT0, and writes luminance to RT1 using `0.2125, 0.7154,
0.0721`. It has no depth, camera, projection or discard. This is an image
copy/luminance pass, distinct from the four-input deferred scene resolve. No
active VR code special-cases these hashes. Its input image still must be
associated with the scene: the conflict witness lacks that SRV identity, so
bytecode alone does not justify preserving the preceding HDR depth/camera
across this write. The next bounded F10 instrument records the actual copy
input/output and prior source/destination provenance together.

The eleven unknown pairs were checked against exact Steam creation blobs under
`C:\Steam\steamapps\common\Elite
Dangerous\Products\elite-dangerous-odyssey-64\edvr_logs\shaders` and the Epic
shader directory. This inventory records algebra/consumers, not live scene
ownership or admission. Existing projection layouts cover the known VS algebra;
no new recipe is authorized solely by this table.

| VS / PS | VS projection | PS consumption / remaining limit |
|---|---|---|
| `A1B7CFCD0BE7493E` / `2DB678B6B558B604` | b2 rows 10-13, DP4 | SV_Position depth t0 Load, G-buffer t1-3, discard, two MRTs |
| `CE24A73943632F55` / `1F64463B15189104` | b2 rows 10-13, DP4 | SV_Position depth t0 Load, G-buffer t1-3, discard, two MRTs |
| `41E245D488BFE83E` / `6EF82262EB12A037` | b0 rows 4-7, DP4 | projected depth t0 sample/discard, material t1-3, b2 direction basis |
| `B12F7A618E1BDE98` / `42AC0CACC9CDF72B` | b0 rows 4-7, DP4 | varying/CB texture coordinates, cube t2; no evident depth compare |
| `203DF51758AADC4D` / `EEAAC839A9F09448` | b0 rows 4-7, DP4 | t0 uses SV_Position scaled by b1[332].zw; t1-6 also sampled |
| `5EAFFCD01B97D0C4` / `DD371C57C9093BB8` | b0 rows 4-7, DP4 | projected depth t0 compare, five discard sites, material t1, one MRT |
| `98397963AAEC45D3` / `CAB49794BB439D03` | b1 rows 270-273, columns | PS creation blob missing in both dump directories |
| `B75A6FF2CA9FA5D6` / `D56F859BE4781431` | VS creation blob missing | PS conditional screen t0 sample; t1/t2 UV samples |
| `124D7F3F649138D4` / `8085AE8DD1906CDC` | b1 rows 270-273, columns; b2[2] alters clip z | varying UV t0/t1, four MRTs, no screen/depth sampling |
| `5C1D8EF529324A22` / `C49F999F7D3C801D` | b0 rows 4-7, DP4 | varying UV t1/t2, fixed tiny t0 loads, b2 effects |
| `820E5C131B99361D` / `6EAA86EFE135B2D4` | b0 rows 4-7, DP4 | projected t0 sample plus t1; no depth compare |

The flat key fix's actual Config regression and full build pass in
`build/flat-profile-jitter-fix.log`. Additional capture code must pass its own
full build before installation; this log validates the key fix only.

The F10 capture now samples exact `CFA91824129ECBBC` / `DFCBA0EC70B03C9B`
before `flatRuntimeObserve`, on two distinct frames at least 90 frames apart.
It reads actual shaders, RT0/RT1/DSV, PS t0 view and resource dimensions, and
viewport under the internal guard. Separate lines show prior source/destination
prefix records, including first writer, aggregate first/last sequence, recorded
depth/camera provenance and prior HDR conflict. Absent records are explicitly
unobserved. No source identity is invented, and scene selection is unchanged.
Summary counters distinguish never observed, shader mismatch, missing records
and rearming. Per-F10 creation-cache requests cover all eleven unfamiliar pairs
and the exact HDR copy; missing caches remain explicit. Periodic jitter logs
expose both temporal enablement and the requested jitter flag.

Review caught the first draft targeting the final output copy instead of the
HDR copy, plus a rearm counter checking the already-reset frame count. Both
were corrected before building or flying. The instrument now checks the
intended exact hashes and does not require the destination to be the final
output. It preserves all admission checks.

The corrected capture and key fix pass the full build and all gates in
`build/flat-hdr-copy-provenance.log`. The maximum conservatively formatted new
log line is 775 characters, below the logger's 1166-character limit. The next
Epic run is for this exact image connection and missing bytecode, not a claim
that temporal AA now resolves this scene.

## 30. Proven HDR image connection and expanded coverage, 2026-09-24

Epic `edvr_gfx_20260924_174156.log` matches `ce715126`, version
`v0.17.0-564-gce715126`, build `6AB5ADB3`, linked 23:09:39 UTC. The key fix
works: `enabled=1 wanted=1`. The renderer accepted 809 frames, each a reset,
with zero continued histories and zero backend failures. Applied jitter
frames/draws/dispatches stayed zero. The last runtime sample reports 18,398
refusals and 2,406,699 jitter refusals. The first detailed jitter refusals
identify unknown projection recipes and one preparation refusal; the later HDR
selector still rejected the depthless image copy.

The targeted probe completed both samples, frames 53307 and 53397, with no
shader mismatch or missing source/destination record. Actual shaders are
`CFA91824129ECBBC` / `DFCBA0EC70B03C9B`. RT0 is the main HDR resource
`0000021C31CA4BA0`, RT1 is `0000021C31CA14A0`, with no DSV. Actual t0 is
`0000021F23884520`: 1920x1080 resource format 9, view format 10, Texture2D, mip
0/count 1, single sample and array element. The actual viewport is full
1920x1080 with depth range 0..1; HDR resource format is 26.

In each sample the input has two earlier writes. The first uses
`CFA91824129ECBBC` / `FCFAD73924BF45B9`, current camera provenance, and the
same depth `0000021C31CA3B20` / DSV `0000021C31303DE0` as the prior HDR writes.
The destination has 20 previous writes beginning with the known deferred
resolve and no conflict. Input write sequences are 1307/1309 and 1214/1216; the
HDR copy immediately follows destination sequences 1338 and
1245. This establishes the sampled connection, but the old aggregate does
not prove the second input write's depth/camera/viewport. Format-9 `hdr-bad=0`
previously said nothing about consistency: checks covered only format 26.

The model now tracks consistency on every format-9 input write. The exact image
copy may continue a previously valid HDR target only when all input writes have
current, matching camera bytes/provenance, depth/DSV/format and full viewport,
with no explicit resource write or source/destination alias. The D3D11 bridge
verifies actual shaders, RT0, absent DSV, t0 view/texture, formats, extent,
sample/array counts and viewport. A successful continuation advances the HDR
write interval while retaining its established camera and depth; the image-copy
shader does not consume its bound b1. A failure keeps the original refusal,
with the offending input-write witness when available. This is shared scene
provenance, not a claim about full overwrite blend state.

Regression cases replay two input writes, the image copy, tone and final copy.
They accept the matching connection and unused copy-camera differences, and
reject second-write depth/camera/viewport changes, missing/stale input,
explicit writes, unverified bindings, aliasing, wrong shaders and previously
bad HDR. A prior HDR failure keeps its original witness even if the input is
also bad.

The 900-frame audit observed 115,823 unknown draws across 32 exact pairs, 89
total outcomes and no outcome overflow. Exact-bytecode review adds 29 of those
pairs (106,762 observations), plus the earlier `98397963AAEC45D3` /
`CAB49794BB439D03` companion now available in Epic. The recipes use existing
forward row/column layouts; `1F17BF54DB6EE407` patches both possible clip
matrices in b1 rows 41-44 and 45-48 in one binding request. No reviewed PS
requires an additional inverse-projection matrix patch.

`B75A6FF2CA9FA5D6` / `D56F859BE4781431` is admitted by its projected anchor
contract: the VS's only b0 reads compute anchor x/y/W from rows 4/5/7.
Jittering these rows moves the divided anchor and depth-sample centers while
preserving W and comparison distance. Its subsequent nonlinear size and
visibility calculations are the response to the jittered projection; uniform
final billboard translation is not the required contract. Focused tests check
anchor/UV displacement and unchanged depth/W. Do not revive a camera-equality
gate or label the billboard inert.

Still missing are PS `51EE1F922FD220B0` (VS `436193B352A2897E`), PS
`D0B9213C1F248335` (VS `F512712C40D93C12`), and both stages of
`CC2BA2E2A927CBD3` / `8A7FB2DB7A33279E`. Image-input PS `FCFAD73924BF45B9` also
lacks a saved blob; its projection/depth behavior remains unproved. Format-9
scene intermediates now enter projection coverage checks, so unknown inputs
refuse jitter warm-up instead of being silently ignored. During F10, each newly
observed unknown pair requests exact cached creation bytes once, bounded to 64
pairs per arm with explicit overflow and missing-cache reports. This also
captures later input writers without guessing their hashes. No new GPU
copy/readback is added.

Ruled out: the fixed jitter setting is still being suppressed, because both
enabled/wanted flags are one. Ruled out: a different depth lineage in the two
sampled first input writes, because their actual resource/DSV matches the HDR
reference. Do not infer coherence of unrecorded writes from those samples; the
new per-write guard establishes it dynamically.

Validation: the complete build and all gates pass in
`build/flat-hdr-continuation.log`, including the new continuation and expanded
recipe tests. Review corrected the prior-conflict fixture expectation and kept
the original destination witness when input validation also fails. In-game
continuation and nonzero jitter remain to be verified on the next Epic run;
missing shader bytes are still an explicit qualification limit.

## 31. Continuation flight and camera-independent input, 2026-09-24

Epic `edvr_gfx_20260924_181446.log` matches `6e9bde74`, version
`v0.17.0-565-g6e9bde74`, build `6AB5B955`, linked 23:59:17 UTC. Final reported
totals: 1,135 accepted / 2,391 refused HDR continuations; 6,487 treated frames,
all reset with longest streak 1; 10,643 refused copies. Jitter is enabled and
wanted but applied frames/draws/dispatches remain zero. The last jitter-refusal
total is 291,478.

F10's 900-frame audit reports 621,282 candidates, 618,456 preparations, zero
preparation refusals, 126 depth-unassociated observations and 96,617 unknown
draws. Automatic capture saw 20 distinct pairs with no overflow. Use the
complete-event outcome lines in this exact log for the next recipe inventory;
do not reuse only the previous flight's list.

The first image-source refusal at frame 37808 names VS `CFA91824129ECBBC` / PS
`FCFAD73924BF45B9`, format 9, correct scene depth/DSV and full viewport, but
b1=null and no camera. Both targeted samples, frames 37899 and 37989, show that
same single input write with no b1. Their HDR destination is otherwise valid.
The current model wrongly requires a camera even for this camera-independent
image pass.

Newly captured Epic `edvr_logs/shaders/ps_FCFAD73924BF45B9.dxbc` was
disassembled with `tools/dxbc_disasm.py`. Its only constant buffer is b2[7]; it
uses UV coordinates, textures t0/t1/t2, gather/filter operations and a discard.
It reads no b1 or inverse projection. Its exact VS passes position and UV
without any constant buffer. A b1 camera requirement therefore tests an unused
binding, which explains intermittent acceptance when that binding was left over
from earlier draws.

Next implementation: add an actual-shader-verified, exact-pair exception to the
format-9 source camera requirement. Keep depth/DSV, format, extent, viewport,
current-frame write tracking and explicit-write invalidation. Geometry source
writes still require current matching camera data; track their reference
separately so a camera-independent first write neither invents a camera nor
masks later geometry disagreement. Test absent and arbitrary unused b1, mixed
image/geometry write order, stale/changed geometry camera, and unchanged
depth/viewport rejection. Mark the exact screen pass projection-inert only
after confirming both hashes; review the other 19 captured pairs from bytecode
before adding recipes.

Ruled out: HDR continuation never executes, because its accepted counter
reached 1,135. Ruled out: this input shader requires a b1 camera, because its
exact bytecode declares only b2 and its VS declares no CB. This does not
justify removing camera checks from other input shaders.

Work stopped before source edits: both delegated tasks hit the account usage
limit. The working tree was clean before recording this entry; the installed
build remains `6e9bde74`. No new flight is needed before the offline work
above.

Sean also asks when the main-menu 3D ship view will stop aliasing. Treat it as
an explicit acceptance scene: verify selected scene, applied jitter and
continued temporal history there. Do not promise menu AA from flight-only
evidence or from backend initialization.

## 32. Camera-independent image writes, 2026-09-24

Work resumed from section 31 without another flight. The exact format-9
`CFA91824129ECBBC` / `FCFAD73924BF45B9` pass may omit camera provenance only
after the bridge verifies actual shader hashes, RTV/DSV identities, texture
dimensions/formats, mip 0 views, single sample/array and full actual viewport.
The model independently checks the exact pair and verified flag. Its unused b1
may be absent or contain unrelated values.

Format-9 geometry writes still require current camera data. The first such
camera is retained separately from the first image write, reusing the target's
existing record storage to avoid adding another large per-target record.
Subsequent geometry cameras must match it, and HDR continuation compares it
with the destination's established camera. An all-image input relies on its
current-frame writes and matching scene depth/DSV; it does not invent a camera.
Depth, extent, viewport and explicit-write invalidation remain in force.

Regression cases cover all-image input with absent/arbitrary b1, both mixed
write orders, stale/changed geometry cameras and unverified/wrong pairs. A
shader's bound but unused camera is not a reason to reject its image write.
Main-menu qualification remains explicit: verify applied jitter and continued
history there, not only in flight.

All 20 unknown pairs (96,617 draws) from the verified `6e9bde74` F10 capture
were inspected together. Eighteen use VS b1 forward columns 270-273, with no
additional projection consumer in their exact PS companions. The image pass
above is explicitly unchanged. The remaining `4AEC439CEC7FFDCE` /
`87EF79B19297B8C4` pair reconstructs a scene-depth ray from VS b1 rows 144-147.
It uses the existing inverse-screen-ray correction: subtract the jitter-scaled
rows 144/145 from row 147.xyz, preserving row 146 and every w component. The
regression verifies the same ray at the shifted screen coordinate. These are
additional measured shader bindings for existing jitter math. Two older
uncaptured pairs remain unknown; this flight introduced none left unclassified.
Runtime handling of an unseen pair still fails warm-up safely.

Validation: full absolute-path `build.bat` completed successfully, including
all 79 jobs, three quiet runs, Python self-tests, flat temporal tests, config
contract and installer resource checks (`build/flat-image-camera-fix.log`). The
changes stay on `codex/flat-temporal-aa`. After committing, rebuild for a clean
identity and install/verify the flat profile on Epic with settings preserved.
Next run captures the menu ship and flight separately with F10; the acceptance
evidence is applied jitter and sustained temporal history.
