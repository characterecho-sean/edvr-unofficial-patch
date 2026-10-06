# Shared temporal AA for VR and flat Elite

## Status

- **State:** native DLAA/FSR work at the settlement; SS above 1 on foot was
  refused by two gates (16M H bound, TAA's native-size rule), both fixed, plus
  the copy-route view; TAA best-of-four history depth NOT FLOWN (104).
- Established or qualified: camera ownership/jitter (26-28); F8 and menu
  treatment (34-37); cockpit projection and smoother DLSS edges (40-43); PS91
  motion ownership and rigid BFE shell motion (49-51); on-foot weapon camera
  and scoped admission (56-57); tone admission, FLOWN at the EDHM main menu
  (63-64); one-raster-phase local refusal, FLOWN in 69 (streak 3349, DLSS,
  zero unknown-pair captures); gate 1 contract and reducer (70, 73; 6/6 traces
  replay byte-identical); gate 2 routing, retirement, negotiation (72, 74;
  04:58 and 09:55 2026-09-27 flights FLOWN); six review fixes (77, no flight).
- Open in the journal: section 75's confirming flight (13:00 storm-free; the
  0.5x canopy flicker is a separate family, CLOSED 2026-09-27); section 76's
  gate-2 matrix cells; section 78: nine of ten refusing ship pairs reciped
  (census 46), vs_C7FA0C0F5DD49180 refused until its blob is captured; the
  upstream camera hook is now design-flat-camera-integration.md.
- **Priority (Sean):** performance over code sharing; honor configured backend.
  Mixed cameras must not downgrade DLAA/FSR to TAA. Share math/backends where
  cheap; separate scheduling/capture when it saves copies/sync/per-draw work.
  Defer broad core extraction until flat capture establishes the boundary.
- **Recommendation:** two installer artifacts, one graphics implementation, one
  temporal pipeline, separate VR and mono frame adapters. Flat installs enable
  only temporal AA and its required support services.
- **Open:** station/on-foot projection coverage and mixed-camera HDR ownership,
  corona-smear regression and mod effect ordering. Scene/depth identity, camera
  encoding and handoff have flight evidence; correct motion for every rendered
  surface and VR regression remain unqualified.
- **Ruled-out pointer:** the kinematic arc's Status records rejected motion
  estimates and the nonexistent engine velocity buffer. Reuse engine-record
  motion; do not revive estimation or the retired deferred UI replay.
- **Ruled out (103-104):** dormant SRC1 false rejection; forced-early UAV
  capture; raising the 64-draw/64-record bounds; a weapon-only cause.
- **Next:** one flight on foot (104): SS 0.75 (copy-route view), 1.0, 1.5 (H
  qualifies, backend runs), EDVR's TAA standing still at the roof. Retain
  102's color-clear fix; no per-weapon table; preserve Epic settings, 87's
  native FSR comparison, 83's open items and high-G motion; do not repeat
  qualified PS91/BFE or stale-resize hypotheses. Menu hangar-floor P1 open; VR
  regression tests and `d9f86b09`'s concourse NPC belong to main/openxr-perf-gaps.
- **Test target (Sean):** all in-game tests on the Epic install under
  `C:\Program Files\Epic Games\EliteDangerous\Products`; keep its INI.
- **Field reports (79-83):** users 1-2 refused every frame, 3 at 7-13 fps, 4
  lost ~23 ms (ReShade). 80-81 flown. 82: (f') CONFIRMED; fix FLOWN, DEFAULT ON.
  10-01 BUILT, NOT FLOWN: the vscreen auto-fit (3504 on Sean's rig: m 0.70 on
  the p10 floor), the curved route, and the cleanup (three experimental keys
  retired, the route auto and the maps gate on by default; sections 82, 84).
  83 (an rc.5 user, SS 0.85 on a 16:10 screen): flat R < D by structure, the
  real cause in the messages, a VR Supersampling warning: BUILT 10-01
  (claude/flat-upscale), NOT FLOWN; its F8 text now waits 2 s before it
  changes cause (a loading screen's size flipped it five times in 9 s).
- **Jitter phases (section 84, 2026-10-01):** the phase-count switch flew with
  no visible change on the hills and is REMOVED (fixed eight phases, byte-
  identical). ROOT CAUSE of the VR hills shimmer, ruled in: Elite's terrain
  checkerboard rendering (halves distant terrain's horizontal samples; turning
  it off fixed it). EDVR now says so in VR (BUILT, NOT FLOWN).
- **Compatibility:** detail below. **PR72 A/B switches:** retired with `on`
  fixed (section 86). Landing-time building shimmer is unqualified (section 88).

## Status detail (moved out of Status 2026-09-29)

*Note: the section 75 sentence below ("awaiting the confirming flight") is
superseded by section 75's own same-day addendum (the 13:00 flight ran
storm-free; the canopy flicker is the separate scintillation family, CLOSED
live 2026-09-27 evening); the Status summary above reflects that. The Next
text repeats a typo ("qualify it then)." twice) kept as originally written.*

### Compatibility decision and environment (moved out of Status 2026-10-01)

- **Compatibility decision:** the prototype accepts an absent profile
  descriptor as legacy VR so manual installations keep working. An existing
  invalid descriptor disables fixes, preserving forwarding/chaining. New
  installers and developer verification require `edvr_profile.ini`.
- **Environment:** initial qualification is Windows, Elite's D3D11 renderer,
  mono SDR output. Record game/patch builds, GPU/driver, display/render sizes,
  window mode, installed mods and backend DLL versions. Headset/runtime are N/A
  for flat. VR regression records the actual runtime/headset/per-eye size.
  Other colour spaces and rendering routes require separate qualification.

### State (as of section 77)

- **State:** merged to main at `dacb7a56` (2026-09-25, includes main
  `a4cdb045`) after Sean's go-ahead; the caveats below remain the open
  qualification record. Latest analyzed Epic build is `d0898e1b`; section 57
  records the on-foot refusal cascade (the laser-rifle weapon pass's second
  camera vetoed every on-foot frame), its scoped admission, and exact recipes
  for the four residual unknown pairs. Section 56 maps all 30 section-55
  projected pairs, classifies the one screen composite unchanged, and adds the
  bounded on-foot camera probe; the merged tree passed full validation.
  Section 54's five mappings passed full validation and are installed; Sean
  reports good ship/station visuals. Section 53 records the preceding
  menu/cockpit F10 evidence. One live projection failure now has an exact
  cause: first-seen topology with complete source shadow and an already
  prepared private buffer. Cockpit motion/depth capture is complete; menu pool
  snapshots reach the per-family cap. Section 51 qualifies live PS91 motion
  ownership: all 45 changed pixels have exact-depth slots and valid joined
  history. Same-code overlay depth overwrites now match the sampled
  menu/cockpit rejection masks exactly. Guarded depth preservation and
  ready-buffer live plan admission passed full validation and the next flight;
  overlay guards have zero declines and one live plan retarget succeeded.
  Sections 26-28 establish camera ownership/jitter; 34-37 cover F8 and menu
  treatment; 40-43 establish cockpit projection and smoother DLSS edges; 45-48
  diagnose missing ownership and its binding repair. The first loading crash
  did not reproduce on retry; its cause remains unknown. Sections 49-50 qualify
  rigid BFE shell motion and the PS91 register correction. Section 58 records
  the rc.2 on-foot reset storm: unreciped scene pairs poison temporal history
  every frame; the live `fix.temporal_aa` change is ruled out as the cause.
  Section 59 pins those pairs to EDHM's patched pixel shaders (the 21:08
  no-EDHM control accumulates normally at the same main menu) and adds their
  exact recipes; mod-patched shader populations remain an open coverage class.
  Section 62 ships the generic scene-pair classifier answering that class.
  Section 63 (2026-09-26): the main-menu EDHM flight it enabled found every
  frame refusing `no-known-tone-pass` instead -- the tone slot alternates
  writers the exact table never knew, on both the VS and PS axes. Tone
  admission now decouples a tone VS set from a per-PS HDR-slot table, with a
  latent PS0 hdr-routing fix; FLOWN 2026-09-26 (section 64): TAA engages at
  the EDHM main menu, one startup `no-known-tone-pass` all session. FSR/DLSS
  that flight were a build-environment gap (no SDKs in the dev build), not
  code; both pinned SDKs are now on the machine and a full-pass SDK build is
  installed. Section 65 (2026-09-26): flight states reset-stormed on 16 unknown
  scene pairs -- EDHM/tier PS variants of mapped VS families plus the
  mod-patched E904 glare VS, all bytecode-reviewed and exact-reciped, INSTALLED
  on Epic, NOT FLOWN. Section 66: the launch and menu-to-flight conflicting-hdr
  storm matches the online model's hardcoded PS1 tone-HDR read (DE65/9270 bind
  their HDR at PS0) -- fixed in `989f6fd`, FLOWN THROUGH build.bat here and
  INSTALLED. Section 67's per-draw stamp form never activated (the flat
  profile gated its key off) and was unproven by design; section 68
  (2026-09-26) replaces it with one-raster-phase local refusal per the
  review's findings 1/4/5: a per-draw-local refusal invalidates history and
  returns the runtime to observation until a refusal-free frame, the
  stamp/mask/re-issue machinery removed, census and named classifier reasons
  kept. INSTALLED on Epic, NOT FLOWN. The review's staged program
  (three-size routing, FrameContract reducer, cache retirement, family
  contracts, composition tests) is recorded, not started. Section 69
  (2026-09-26): the section-68 flight FLOWN -- menu and flight treat
  continuously (streak 3349, DLSS), zero unknown-pair captures (the
  section-65 recipes hold), one one-frame observation episode on a reciped
  pair. The surviving conflicting-hdr is qualified as transition-scoped
  video/bloom content, not an HDR alias; no relaxation needed. Section 70
  (2026-09-26): the review's gate 1 shipped -- the reducer produces the
  immutable FlatFrameContract at the copy draw with no decision change, the
  trace ring records its inputs and dumps on F10, and the rig replays
  traces to identical contract hashes. FLOWN 2026-09-26: the stock/EDHM
  menu and flight traces replay byte-identical (6/6 frames) and are
  committed as the corpus's first entries. Section 73 (2026-09-26): the
  gate-1 review's three reproduced gaps closed without a flight -- every
  copy outcome recorded and hashed (full semantic selection, camera matrix
  included), the corpus gate hard-fails on missing/unreadable/empty input
  with a manifest pinning scenarios, and the stored hashes regenerated by
  the rig's --trace-migrate under schema EDVRFTR3. Section 72 (2026-09-26): gate 2
  designed (three sizes R/E/D, the routing table, retirement, negotiation);
  step 1 shipped: `flatResolveRoute` names today's effective treatment per
  pairing (honest refusals included), logged on plan change, rig-tabled.
  Section 74 (2026-09-27): the gate-2 review's final-pass pair closed
  without a flight -- the negotiated E keys the resolve's resource cache
  and the preflight carries it; buffer-pressure retirement loops until a
  shared buffer unpins, bounded by the plan bank. Section 75 (2026-09-27):
  the 0.5x canopy flicker traced to success-status Present results
  resetting temporal history every frame; the history and phase gates now
  use FAILED(hr)/SUCCEEDED(hr) with the value logged, awaiting the
  confirming flight. Section 76 (2026-09-27): the gate-2 qualification
  flight matrix is staged -- the 0.5x DLSS menu trace admitted to the
  corpus, the eight-step session script recorded; crops remain deferred
  on the source-rectangle lineage. Section 77 (2026-09-27): all six
  rc-since-rc2 review findings fixed without a flight -- classifier
  depth-output and multi-row texcoord detection, the stamp over-read,
  after-UI exclusion preservation, negotiation-before-eval ordering, and
  the analyzer's legacy-layout gate.

### Next (as of section 68/69)

- **Next:** fly the Epic install on foot in the hangar and concourse. The
  section-57 admission should end the hdr-camera-changed refusal cascade;
  confirm treated streaks resume on foot, watch the weapon itself for local
  rejection crawl. The section-59 main-menu flight happened (section 63):
  zero unknown-pair captures held, but the tone slot refused every frame;
  the widened tone admission is FLOWN for TAA (section 64); the SDK-full
  install confirmed DLSS at the menu (section 65). Next: the section-68 flight
  -- EDHM at current settings, supersampling at most 1.0, main menu with DoF on
  and off, flight and station, then supersampling and resolution changes
  mid-session; expect treated streaks through each change, observing=
  transitions with locally-refused pairs named instead of a reset storm when
  coverage is incomplete, and no conflicting-hdr at launch or menu-to-flight
  (a storm that survives the slot fix is a genuine alias; qualify it then).
  qualify it then). The section-57 on-foot hangar/concourse flight stands
  behind it.

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

> **Deferred, 2026-09-25.** The live copy of this doc is on
> `codex/flat-temporal-aa`. Sean's priority there is performance over code
> sharing: broad core extraction waits until flat capture establishes the
> boundary, and flat got its own mono resolve (that branch's section 26)
> that never touches `temporal_pass.cpp`'s per-eye state. Do not extract
> per-view state from `g_eye[2]` as a standalone change (this paragraph,
> delivery step 2 in section 7); external PR #41 did, and is on hold.

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

## 33. Menu and flight verification of 5c78c34d, 2026-09-24

Epic `edvr_gfx_20260924_185533.log` matches `v0.17.0-567-g5c78c34d`, build
`6AB5C146`. Main-menu F10 at 18:56:24 captured four unknown pairs and 9,000
unknown draws; no exact HDR copy or ready resolve was observed. The runtime's
menu conflict was a depthless 2560x1440 format-26 draw with VS
`DEF19B035D5EDEDC` / PS `DED8796049C7BB4A`.

Flight F10 at 18:57:38 captured 962,537 candidates, 959,687 prepared, zero
preparation refusals and zero unknown scene pairs. Backend and spatial fallback
were ready; exact copy provenance completed 2/2 captures. HDR image
continuation reached 6,767 accepted / 1,763 refused. Still, applied jitter
frames/draws/dispatches stayed zero, phase stayed warming and all 6,583 treated
frames reset history. There were no continued frames.

Ruled out: unseen flight projection shaders explain this run's warm-up failure,
because the second F10 audit has zero unknown pairs. Ruled out: alternating
runtime refusal or depth/color swaps caused the sustained flight resets,
because refusals remain 8,994 throughout 18:57:29-18:58:39 and all adapter
reset causes are zero. The explicit invalid-phase-history reset is responsible.
The 66,254 phase failures are not classified after the global first-12 detail
cap, which was exhausted in the menu. Add a bounded reason census before
changing qualification policy. Passive discovery conflicts do not establish
failures in the continuously treated live runtime.

## 34. Flat onscreen AA controls, 2026-09-24

Sean requested live Off/TAA/DLSS/FSR3 selection and the DLSS model preset. Use
F8, with Up/Down for rows, Left/Right for values and Escape/F8 to close. Reuse
the existing menu's asynchronous INI writer and GDI raster; restrict flat
content to temporal settings and composite on the owned swapchain before
Present. Menu drawing stays after scene AA so its text never enters temporal
history. Closed-menu frames must perform no menu texture copies or dispatches.

Keep the generic stereo `fix.temporal_aa` read suppressed in the flat profile;
the menu reads the same explicit requested mode as the mono adapter. Narrowly
allow `hotkey.menu` and `fix.temporal_aa_model`, retaining every unrelated
feature restriction. Preset names share the existing VR mapping (Auto/J/K/L/M
and legacy aliases), while flat applies changes together with history reset and
resolve preflight invalidation at its Present boundary. No runtime model switch
may happen in the middle of a frame. Selecting a setting is a request, not
evidence that the current scene has qualified temporal treatment.

All four new menu shader pairs have complete captured contracts. Add exact
recipes: `61AE8EB05FDC18DD/4504BC268E109C31` uses VS b1 columns 270-273;
`0357BBB2DEE43C1F/222188632125D14B` uses VS b2 dp4 rows 10-13;
`4EF6DDB075A927FA/098C0764D28FC42C` and `95D01BA609BF7500/F10792B40AE3ED42` use
VS b0 dp4 rows 4-7. Their pixel shaders need no additional inverse correction.
The second pair derives its view ray from interpolated VS output transformed by
b2 rows 2-4, which follows the jittered geometry; the PS samples scene depth at
that same raster pixel. These recipes do not address the separate depthless
menu HDR refusal.

The phase-failure census holds at most 32 reasons, with explicit overflow.
Every five seconds it reports calls, distinct frames and frames that were
ultimately treated, plus first/last frame per reason. A zero-failure summary
distinguishes successful collection from an instrument that never ran. Frame
accounting finishes before the next frame clears its prefix; resize/stop
flushes the remaining window. No allocation or routine log write occurs at the
per-draw failure site.

Validation: `build/flat-menu-validation-3.log` passes the full build, all 79
jobs, three quiet runs, config contract and installer-resource gates. The menu
WARP suite passes 98 checks, including hidden/visible pixels, unchanged pixels
outside the panel, graphics-state restoration, no retained backbuffer
reference, resize and a replacement device. An earlier run caught the flat
identity matrix using an interleaved 3x4 layout instead of the compositor's
packed 3x3 plus translation; that was fixed without weakening the tests. The
settings parser covers profile restrictions and the shared preset names.

The flat package documents F8 and the two settings. Its default mode remains
off. Commit and rebuild with a clean identity, then install/verify Epic with
the live INI preserved. In-game keyboard navigation and mode/preset switches
still need the user's next run; EDHM/ReShade effect ordering remains part of
the broader flat qualification. No merge to main.

## 35. Menu mode cycling without treatment, 2026-09-24

Epic `edvr_gfx_20260924_192550.log` matches `v0.17.0-568-gbd30cbdc`, build
`6AB5CCC3`. Sean sees no visual change while cycling AA modes in the menu.
Saved settings and runtime transitions confirm Off, TAA, DLSS, FSR3 and DLSS
preset changes arrive. Renderer calls, allocations, treated frames, applied
jitter and continued history all stay zero. This run cannot qualify any
backend: scene selection refuses before backend execution.

Ruled out: mode changes were ignored, because each saved setting has a matching
runtime transition. Ruled out: a backend execution failure explains this menu
run, because no backend was called. The first format-26 HDR write has VS
`DEF19B035D5EDEDC` / PS `DED8796049C7BB4A`, with no depth resource or DSV at
3840x2160 and later 2880x1620; refusal is `missing-depth-or-dsv`, followed by
`conflicting-hdr-target-or-camera`. Its bound b1 is current but unused: the
captured VS passes position/UV unchanged and the PS only samples t0/s0. Bound
camera data therefore cannot justify inheriting scene depth.

F10 reached the collector at 19:27:53.845. AA had been switched off at
19:27:50.489, so the live projection audit returned before consuming its arm
request. Passive discovery and both focused compute samples did run. Their
scene association is refused, and the focused-report policy suppresses the
retained contract/source details. Missing projection captures do not mean the
hotkey failed. Neither this log nor the preceding menu capture establishes the
copy's actual PS t0 resource or its last same-frame writer.

The next instrument must distinguish an earlier depth-bearing graphics source,
a compute-produced source and incomplete writer evidence in one capture. Record
actual copy bindings and bounded source lineage independently of AA selection,
including missing/overflow outcomes. Do not relax the depth gate or reuse an
arbitrary bound camera on the strength of shader identity alone.

The new passive probe samples only the two F10 focused-compute candidate
frames, up to four exact menu-copy draws each. It records actual shader, PS t0,
RTV0, DSV and viewport getters, resource/view layouts and preceding retained
graphics/transfer observations at the copy sequence. Its report runs at that
candidate's Present regardless of scene selection, before frame data is
cleared; it does not depend on the separate five-second passive-report timer.
This permits same-frame comparison against existing compute UAV/source records
while AA is off. Zero-copy, overflow and incomplete writer evidence remain
explicit. Bound-but-unused camera data is still not a scene contract.

No AA admission or rendering behavior changes in this diagnostic build. Next
Epic test: select DLSS with F8, close the panel, press F10 at the menu ship and
remain there for 30 seconds. No flight is needed to capture this blocker.

Validation: the final full absolute-path build passes all 79 pooled jobs, three
quiet runs, Python self-tests, config contract and installer resource gates
(`build/flat-menu-copy-validation-final.log`). Review verified getter reference
release, reset between the two samples and reporting before frame clear even
without a selected scene. The summary explicitly labels its shadow shader
filter; actual getter hashes must still match before drawing conclusions.
Rebuild after committing for a clean identity, then install/verify Epic with
the current live INI preserved. Keep this work on the feature branch.

## 36. Menu HDR source connection and compact controls, 2026-09-24

Epic `edvr_gfx_20260924_200952.log` matches `v0.17.0-569-g1aa62d94`, build
`6AB5D225`. The menu remains untreated. Both new probe frames, 34536 and 34980,
verify actual VS `DEF19B035D5EDEDC` / PS `DED8796049C7BB4A` and a full
2880x1620 viewport. PS t0 is resource `26A3E8C20`, copied to `28BCA1020` with
no DSV. Both are format 26, typed 2D views at mip zero, single sample/array.

The source has 30/31 preceding graphics draws, q672..811/823, with depth
`26A3E8120`, DSV `2360071E0`, format 19 and the same extent. The final graphics
pair is `94D5C556DFD6D705` / `912477AEF6958379`, with a same-frame camera write
at q805/817. Copy q815/827 follows it. In the same sampled frames, lighting CS
`5998146D464F5C0E` and `EB0245DE0BB23BB6` write that source as UAV0 at q686/690
while SRV4 names the same scene depth. The existing runtime permits HDR
lighting writes before tone mapping. All reported passive coverage-drop
counters are zero. Resource suffixes here are meaningful only within each
sample; the repeated addresses are not cross-frame proof.

Ruled out: the menu copy has no observed scene/depth connection, because both
actual-binding samples identify its earlier depth-bearing HDR source. Still
unproved: the source satisfies every runtime camera and write-consistency
check. The passive aggregate does not expose its `hdrBad`/`hdrCamera` state.
The implementation must require those checks and preserve their first refusal
witness, rather than presume that a matching texture already qualifies.

The narrow transfer contract verifies the exact shaders, actual source and
destination views, extent and viewport, then carries a valid current-frame
source's depth and camera through its first copy into a new HDR destination.
The copy's unused b1 is never the scene camera. Unsupported copies, stale or
conflicting sources and prior destination writes continue to refuse. Existing
source selection, projection qualification and history gates remain in force.

Sean also requested a smaller F8 menu at the upper left. Interpret one quarter
of its size as half the displayed width and height, with a small corner margin;
the flat layout changes independently of VR menu placement.

Implementation keeps the destination write at the actual copy sequence and
retains the source camera's original write epoch/sequence. It pins the
inherited depth resource through Present. A rejected first copy is still
recorded as a write, ensuring the final selector reports its conflict instead
of losing the diagnostic as an absent HDR target. Source refusals preserve
their original witness; an earlier destination refusal takes precedence.
Separate five-second menu-copy counters distinguish transfer acceptance from
temporal treatment.

Later compute writes invalidate inherited destinations even before tone
mapping. Existing source lighting before the copy remains allowed. The extra
lookup is skipped until a menu copy has been accepted in that frame. Regression
cases exercise this ordering, source camera/depth/layout validity, unused copy
camera bindings, missing or unverified inputs, prior destination writes and
failure-witness propagation.

The compact panel halves the previous width and height after its height cap,
with a two-percent margin from each upper-left edge. Placement uses swapchain
dimensions, independently of the game's SS setting. The existing WARP suite
checks visible pixels near that corner, alongside hidden-menu, graphics-state,
resize and device replacement checks. VR geometry remains unchanged.

Validation: full absolute-path `build.bat` passes all 79 pooled jobs, three
quiet runs, Python self-tests, config contract and installer resource gates
(`build/flat-menu-lineage-validation.log`). The flat temporal policy tests
pass, and the menu GPU suite passes 99 checks. Rebuild with the committed
identity, then install/verify the flat package on Epic with the live INI
unchanged. Next run checks the compact F8 panel and captures 30 seconds at the
menu ship with DLSS selected and F10; confirm menu transfer counters, actual
treatment, projection phase and continued history rather than assume visual
qualification.

## 37. Live menu AA verification and main merge, 2026-09-24

Epic `edvr_gfx_20260924_202704.log` matches `v0.17.0-570-gcada07f0`, build
`6AB5DB54`. Sean reports a slight shimmer difference while cycling AA and
identifies the remaining shimmer on ship surface details. This is the first
verified menu run with sustained treatment: 5,745 renderer calls, 5,714
accepted history continuations, 31 resets and zero backend failures. The
longest uninterrupted continuation is 1,826 frames. Menu HDR transfers total
5,760 accepted and zero refused. Render size is 2880x1620 with 3840x2160
output.

DLSS initialized and evaluated, and the later FSR switch initialized its
backend at the same dimensions. Off, TAA, DLSS and FSR changes reached the
runtime. DLSS J/L/M/Auto/K preset changes applied at frame boundaries and reset
history as intended. The final stable DLSS interval has live jitter and zero
phase failures. The brief engine-source refusal coincides with switching off,
not sustained scene rejection.

F10 at 20:27:46 captured 900 projection-audit frames: 316,084 candidates were
prepared with zero refusals and no unknown shader pairs. Passive discovery's
separate selector still reports a conflicting HDR target because it does not
implement the live copy transfer. Do not confuse that diagnostic with live
runtime failure. The log explicitly lacks per-pixel motion counts; engine
record-join counts alone cannot establish a motion or coverage defect.

Ruled out: menu AA never engages or continuously resets its history in this
run, because live jitter, backend evaluation and sustained history are all
observed. Remaining surface shimmer needs matched image and pixel/motion
evidence before changing reconstruction, sharpening or thresholds.

At Sean's request, fetched and merged main `c9cab91e` into
`codex/flat-temporal-aa`, keeping the feature branch separate. Git merged
`build.bat`, `temporal_pass.cpp` and `vscreen.cpp` automatically with no
conflicts. Review retains flat production sources, rigs, installer/profile,
menu and mod-chain paths; main's new eye rendering work stays on its VR path.
The merge's required validation is a full absolute-path build. Main adds a
receipt-guarded `build.bat --dll-only` promotion step after that validation and
commit, so the installed DLLs can carry the clean commit without rerunning
unchanged test rigs. Epic settings must remain unchanged.

## 38. Cockpit viewport rejection witness, 2026-09-24

Epic `edvr_gfx_20260924_203853.log` matches `c38f6946`, build `6AB5DE09`. The
merged build passed full validation and receipt-guarded promotion before
installation. The cockpit scene renders at 1920x1080 with 2560x1440 output;
this is flat D3D11 with DLSS selected, so headset and VR runtime are N/A.

F10 at 20:44:36.845 completed its 900-frame audit at 20:44:46.906. It records
140,137 candidates, 137,269 prepared and 168 depth-unassociated candidates. The
remaining 2,700 are exactly three viewport failures per frame. Five-second
phase summaries independently report 1,350 viewport failures in 450 frames,
zero jitter, no history continuation and a reset on every treated frame. There
are no unknown shader pairs during this audit. Unknown-recipe failures after
20:45:02 are outside the captured interval.

Ruled out: missing projection recipes explain this captured cockpit failure,
because the bounded audit reports zero unknown pairs and identifies viewport
qualification as the phase failure. Backend execution alone does not qualify
temporal AA when history resets every frame.

Leading hypothesis: the three full-extent HDR passes with depth range 0..0 seen
in older Epic captures are rejected by projection qualification's 0..1
requirement. Their VS/PS pairs were F8FA801F2CB1E27C/84965D3C050FB01B,
68DDDEF04D9894AF/06332CA168B6DA63 and F7A6E916F14A3B1A/06332CA168B6DA63. The
scene selector already allows this HDR depth range. However, the current
capture does not identify the rejected shaders or actual viewports, so the
matching count is not sufficient evidence to change the rendering rule.

VR comparison: `OpenVRSystem::GetProjectionMatrix` and `GetProjectionRaw` apply
the runtime's tangent shift before returning projection values to Elite. Flat
has no corresponding runtime query, so its scoped D3D11 constant-buffer patches
need draw qualification. The shared jitter sequence, motion math and temporal
backends are retained; this mismatch is in the additional flat draw
qualification, not evidence that the VR reconstruction needs rediscovery.

Other possibilities are a viewport extent/origin mismatch or a viewport count
other than one. The diagnostic must record shader identity, actual viewport
count and all six fields, expected extent and resource identities at the
rejection. Per-pair totals and separate witness/overflow counters must survive
the ordinary detail-log budget, reset on F10, and report even when no viewport
checks fail. Keep the existing rejection rule and history policy unchanged.

Next Epic run: select DLSS in the same cockpit scene, press F10, and remain
there for 30 seconds. Confirm the installed build first. Use the witness to
distinguish depth-range, extent/origin and count failures in this one capture;
do not request separate flights for these hypotheses.

## 39. Confirmed cockpit HDR depth range and menu quality, 2026-09-25

Epic `edvr_gfx_20260925_045229.log` matches `f6c59ba6`, build `6AB5E2B9`.
Environment: flat D3D11, RTX 5090, DLSS Quality preset K, 1920x1080 input to
2560x1440 output (game SS 0.75 per axis). Headset/runtime are N/A. Driver and
DLSS runtime versions are not reported by this build.

Cockpit F10 started at 04:56:14.579 and completed at 04:56:24.632. Frame 44833
records q365 F8FA801F2CB1E27C/84965D3C050FB01B, q367
68DDDEF04D9894AF/06332CA168B6DA63 and q368 F7A6E916F14A3B1A/06332CA168B6DA63.
All three witnesses report one viewport, (0,0,1920,1080,0,0), with the actual
DSV resource matching both named and phase depth. Each pair has 900 full-XY
depth-clamped failures and zero other failures. Aggregate: 123,958 viewport
checks, 121,258 matched, 2,700 failed; three witnesses, no suppression or
unrecorded outcomes. The projection audit has 125,878 candidates, 123,058
prepared, 2,700 refused, 120 depth-unassociated and zero unknown pairs. Live
phase stays zero and every treated frame resets.

Ruled out: wrong viewport count, XY extent or origin explains this cockpit
capture, because every rejection is exactly one full-size viewport with depth
clamped to zero. The suspected three HDR pairs are now identified directly, not
inferred from matching aggregate counts. Their projection recipes already
exist. This requires correcting viewport qualification, not adding shaders or
reworking motion reconstruction. HDR admission already supports depth 0..0;
motion-source selection must retain its 0..1 requirement.

The correction reuses the selector's HDR viewport predicate for known,
actual-shader-verified projection recipes on format-26 non-output targets with
an RTV and matching, owned scene depth/DSV dimensions. Other target roles
retain the full 0..1 viewport predicate; compute is unchanged. The raster
viewport is still queried from the actual context, and all existing shader,
constant-buffer, extent and phase checks remain. No new shader allowlist is
introduced. Scalar viewport overloads avoid copying a full draw observation.

Menu screenshots at 04:54:07 (DLSS K) and 04:54:27 (Off) show remaining hard
edges around the ship canopy and surface detail; the crops differ in framing.
At 04:54:04, 09 and 14, each five-second DLSS interval has 450 history
continuations, zero resets and live jitter. The mode switches off at 04:54:14.
Thus menu frame-level continuity is established, but per-pixel DLSS
contribution is not. `flat_mono_shader_source.h` finish substitutes bilinear
current colour when any pixel of the 2x2 input footprint has rejected motion.
This safeguard could explain untreated edges; it is not yet proven at the
pictured pixels. Preserve the safeguard until matched colour, motion,
rejection, pre-finish DLSS and final-output evidence identifies the cause. SS
1.0 with DLSS already selects native DLAA; it is an optional quality comparison
with about 78 percent more input pixels than SS 0.75, not a fix for a possible
integration defect.

Next Epic run after the viewport correction: keep SS 0.75 and DLSS K for the
same cockpit F10 capture. Verify the three passes prepare, viewport mismatch
counts stay zero, jitter becomes live and history continues. Watch camera turns
near the star for the previously reported corona smear. Further menu quality
investigation is separate from this confirmed cockpit blocker.

## 40. Cockpit viewport fix verified; capture timing gap, 2026-09-25

Epic `edvr_gfx_20260925_050806.log` matches `0846a1f7`, build `6AB655A8`. The
05:10:03.479 F10 audit completes at 05:10:13.505: all 136,260 viewport checks
match, 138,060 projection candidates prepare, zero are refused, 186 are
depth-unassociated and zero unknown shader pairs are observed. Each of the
three formerly rejected HDR shader pairs prepares in all 900 frames. Live
jitter and history continuation persist through 05:10:17.204; the longest
uninterrupted history run reaches 1,757 frames. Backend failures remain zero.

At 05:10:17.938, frame 36424, a new unknown projection draw invalidates the
jittered frame, which is recovered spatially. Subsequent frames reset history
with zero jitter: exactly one unknown-recipe failure per treated frame. This
persists while passive selection still finds the same scene HDR/depth/camera
buffer resources at 1920x1080, with 2560x1440 output. The scene exit is later,
around 05:10:48.502, so this is not just an exit-transition artifact.

Ruled out: the depth-clamped HDR viewport still blocks cockpit accumulation,
because all three passes prepare and every audited viewport matches, with
sustained live jitter and history. The later refusal has a different reason.

The diagnostic had a timing gap: `captureUnknownProjection` returned whenever
the 900-frame audit was inactive. At about 90 fps, the detailed audit lasted 10
seconds despite asking Sean to remain for 30 seconds. This unknown arrived
4.433 seconds after the audit ended. Later passive payloads were suppressed by
the focused compute probe, and there are no DC/DCO census lines. No exact VS/PS
identity or creation bytes for this failure can be recovered from the log or
on-disk shader cache. One creation-cache drop is reported; availability of
unidentified shader bytes must not be assumed.

Next diagnostic: collect bounded, deduplicated first-seen unknown projection
pairs even outside the manual audit, request retained exact bytecode once, and
identify automatic versus F10 evidence explicitly. Keep shader admission and
history refusal unchanged. This avoids depending on F10 timing to find a new
material. Next Epic run keeps DLSS K and SS 0.75, and includes the view or
motion that exposed the later failure. Visual quality and the menu rejection
mask remain separate open questions.

## 41. Automatic capture identifies decal projection, 2026-09-25

Epic `edvr_gfx_20260925_052051.log` matches `85bf7652`, build `6AB65852`.
Automatic evidence capture retains three unknown VS/PS pairs, saves all six
bytecode stages and reports no failures, absent stages or capture overflow. One
creation-cache drop is unrelated to these successfully retained blobs.

At 05:21:48.932, frame 26972, captures identify
989E043933A369AB/CE844D87026C684C at q4 on format 23 and
525D47E3D5E2EFF4/F0BAE053476F8730 at q11 on format 26. These occur in an
interval with no accepted tone pass and no temporal treatment. Keep their
failure counts separate from the subsequently treated cockpit scene.

The cockpit runs with live jitter and continued history from 05:22:36 onward,
apart from one preparation refusal at 05:22:37.651 recovered spatially. It
reaches 3,279 consecutive history continuations, about 36 seconds at 90 fps,
with zero backend failures. At 05:23:14.158, frame 34466 q265, the new pair is
0C4E76889907B963/A90825082F36756E, on format 23 at 1920x1080 with the named and
phase scene depth and full 0..1 viewport. Its VS and PS blobs are 2,364 and
6,844 bytes. The first jittered frame recovers spatially; 68 subsequent treated
frames reset history before the scene has no accepted tone pass. This run does
not establish that the unidentified draw in `0846a1f7` was the same shader;
that earlier identity was never captured.

Ruled out: first-occurrence evidence still depends on F10 timing, because all
three pairs and all six shader blobs were captured automatically without a
manual audit. The remaining refusal now has exact bytecode to inspect offline.

Disassembly of all six captured blobs establishes three existing recipes:

- 0C4E76889907B963/A90825082F36756E is decal scene geometry. VS instructions
  37-43 form clip position from CB1[270..273]; 44 copies clip XYW to the
  pixel-stage screen-coordinate varying and 45 writes SV_Position. The PS
  divides that varying to sample scene depth and reconstructs from the
  world-relative ray. Its CB1[277..279] uses are view direction/normal
  rotation, not an inverse projection. Vertex b1 ForwardColumns at row 270
  moves both raster position and depth-sampling coordinates coherently.
- 989E043933A369AB/CE844D87026C684C uses VS instructions 110-113 to write
  SV_Position from CB0[4..7] by dp4. The PS uses material UV and orientation
  rows, with no separate projection consumer. Reuse Vertex b0 ForwardDp4 at row
  4.
- 525D47E3D5E2EFF4/F0BAE053476F8730 is a texture-neighborhood filter. Its VS
  has no constant buffer and maps UV to clip position; the PS samples the
  texture footprint with position-to-UV scale and material/exposure constants.
  Classify this exact pair as unchanged, without inventing projection rows.

All additions require the captured PS companion. No scene-source or engine
motion family is added. Regression checks cover both projection recipes,
unobserved companion rejection, the unchanged pair and decal screen-UV/raster
jitter agreement. Next Epic run keeps DLSS K and SS 0.75 and repeats the same
cockpit activity; automatic evidence remains available for any further unknown
pair. Menu pixel rejection is still unmeasured and is not changed here.

## 42. Cockpit A/B blocked by a projected effect, 2026-09-25

Sean reports no visible cockpit geometry aliasing difference between Off and
DLSS. Epic `edvr_gfx_20260925_053405.log` matches `85d590e0`, build `6AB65BC4`.
At 05:36:09.063, frame 31376 q574, automatic capture saves
2D8263CC54D55398/89B662E266E5D73E on format-26 HDR, 1920x1080, with owned scene
depth and full 0..1 viewport. Both blobs are 1,120 bytes and their repository
FNV hashes match the recorded identities. This is the only newly unknown pair,
but it executes six times per frame: 2,700 failures per 450 frames. The prior
three additions do not reappear as unknown pairs.

The A/B switches DLSS to TAA/Off at 05:37:30.489/30.708 and back through TAA at
05:37:33.864 to DLSS at 05:37:35.686. Before and after those changes, jitter is
zero, history is invalid and each treated frame resets. For example,
05:37:38.875 reports 448 resets and zero history continuations, followed by 450
resets and zero continuations at 05:37:43.875. Backend failures stay zero. The
selected menu mode therefore does not establish effective temporal AA.

Ruled out: the reported cockpit A/B proves DLSS reconstruction cannot improve
the edges, because temporal history was continuously reset during that A/B.
This is another observed qualification blocker; menu per-pixel rejection
remains a separate unconfirmed hypothesis.

Full bytecode inspection proves the existing Vertex b0 ForwardDp4 row-4 recipe.
VS instructions 3-5 compute clip XYW from CB0[4], [5] and [7], then 6-7 copy
the same values to the PS varying and SV_Position. Instruction 20 uses CB0[6]
for Z. The PS divides that varying at 0-2, samples depth at 3, and compares
against unchanged clip W at 4-6. Remaining instructions 7-26 use view-position
length, fade, colour and exposure, with no inverse projection. The effect needs
coherent raster/depth-UV jitter and must not be classified unchanged. Add only
the exact captured pair to the existing recipe, preserving local rows 9-11,
clip Z/W and all scene admission checks.

Next Epic run keeps SS 0.75 and DLSS K in the same cockpit view. Confirm
sustained history during the A/B before judging edge quality. Automatic capture
continues to retain any further unknown shader; no timed F10 is required for
that evidence.

## 43. Sustained cockpit accumulation verified, 2026-09-25

Epic `edvr_gfx_20260925_055040.log` matches installed binary `b494e087`, build
`6AB65FCC`. DLSS initializes at 05:51:22.506 with 1920x1080 input and 2560x1440
output, Quality preset K. The session has no recorded mode switches. Sean
confirms the cockpit edges are clearly smoother with DLSS, but reports some
edge shimmer as the light changes.

After scene entry around 05:52:50.951, cockpit jitter is live and history
valid. Every five-second interval through 05:53:25.954 has 448-450 history
continuations and zero resets. The final summary reaches 3,310 consecutive
history continuations, about 37 seconds, with zero backend failures. Automatic
unknown capture reports zero distinct pairs throughout; the old six-draw
projected-effect refusal is absent. Whole-session totals are 4,153 renderer
calls, 4,150 continuations and three resets, including the earlier menu and
scene-entry history starts.

There are isolated earlier preparation/no-raster refusals outside the steady
cockpit interval. At frame 42002, the tone pass disappears and one spatial
recovery is logged before the scene no longer qualifies. Those transitions do
not indicate ongoing cockpit resets.

Ruled out: the captured projected effect still continuously blocks temporal AA
in this run, because no unknown pairs are observed and live cockpit history
continues for 3,310 frames. The counters establish runtime continuity; Sean's
feedback establishes visible smoothing. Neither establishes complete
pixel-level motion correctness or coverage of unseen scenes/shader families.

Keep the installed `b494e087` binary. Remaining lighting-dependent shimmer
needs matched current colour, motion, rejection mask, raw DLSS output and final
composite evidence to distinguish reconstruction from replacement of rejected
pixels. Do not change the existing rejection safeguard without that evidence.

## 44. Lighting-dependent edge shimmer: discriminating capture, 2026-09-25

The verified history continuity and visible smoothing in section 43 remove
continuous frame resets as an explanation for this run's remaining shimmer. Two
pixel-level hypotheses remain open:

- Final-composite rejection restores untreated edge pixels. The finish shader
  maps output pixel centers through the current raster jitter, takes the
  maximum rejection over the four neighboring input pixels and substitutes
  bilinear current color wherever any rejects. Evidence: raw DLSS is smoother
  than the final image at pixels selected by that exact mask.
- Shimmer is already present in raw reconstruction. Evidence: the same edge
  changes in raw and final DLSS output where the rejection footprint is zero.
  Motion alignment, changing highlights and subpixel coverage then need
  examination; a healthy whole-frame history counter cannot establish them.

Lighting is not an input to the prep rejection decision. That decision uses
engine record/depth ownership, camera reprojection and validity/bounds checks.
Changing light may reveal an existing rejected edge without changing its
classification. The existing LDR DLSS input uses unit exposure/pre-exposure and
render-pixel motion; this audit provides no evidence to change those settings
or weaken the safeguard that prevents invalid-history smearing.

The diagnostic copies matched current color, prepared depth, motion, rejection,
raw DLSS and final composite textures already present after the finish
dispatch. Manual F10 arms four samples separated by at least 15 frames, with
one pending staging set, a 384 MiB total cap and explicit expiry/error
reporting. Readback must be nonblocking; it must not reset temporal history or
alter rendered output. Each complete sample carries frame, jitter, dimensions,
mode/reset and native texture layout metadata. An offline tool reconstructs the
exact output-space rejection footprint and compares raw/final images.

Before flight: verify copy/readback and row layout on the D3D test device,
inactive/expiry/rearm behavior, offline mask mapping including fractional
jitter and clamping, manifest integrity and dry-run non-mutation. Full build
and clean-commit promotion remain required. The next capture must report
complete samples or explicit failure; absence of files is not a successful
diagnostic. Captured samples can distinguish the two paths but do not measure
all temporal stability or prove the underlying motion correct.

Sean requested another main merge before this build. Merge `origin/main`
`d87f40b2` into the feature branch, including its camera-row carry correction,
hologram depth work, preset row visibility and quiet JSON self-tests. The sole
conflict is in `menu.cpp`: retain flat requested-mode labels and flat row
selection when deciding preset availability, while adopting main's separate dim
flag so highlighted inactive preset text stays dim. The compact flat panel
remains. Full validation covers the combined tree; this is not a merge of the
feature branch into main.

Use F10 while DLSS is active and the shimmering edge is visible, then leave the
scene running for a few seconds. The pixel burst ends after four samples (or an
explicit cap/timeout); the existing draw audit can keep running. No rendering
setting is changed. A normal run without F10 does no pixel capture. Output is
beneath the configured log directory's `flat_pixels` folder. Keep the session
directory named by `flat pixels: armed/completed` and run:

```text
python tools/flat_pixels.py <session-directory>
python tools/flat_pixels.py <session-directory> --output <preview-directory> --dry-run
python tools/flat_pixels.py <session-directory> --output <preview-directory>
```

The first command only reports statistics. PNGs include current color, raw
DLSS, final composite, an output-space rejection overlay and amplified
raw/final differences. Preview alpha is forced opaque; native RGBA remains
untouched. Depth and motion are retained in their native formats for follow-up
analysis. The analyzer uses NumPy and validates manifests/lengths before
reading. Producer/parser agreement is checked with actual D3D readback in the
existing mono resolve test rig, in addition to the offline self-tests.

Targeted validation passed in `build/flat-pixel-targeted.log`: capture policy
and WARP resolve, including exact packed rows for all six textures, atomic
manifest publication, rearm/expiry/cancel and the D3D debug-message check. The
analyzer verified the actual producer's 17x3 fixture: 51 rejected pixels, 51
raw/final differences, maximum byte difference 254 and mean 65.5. The build
gate follows a fresh `current_fixture.txt` written only after the producer
passes, so old session directories cannot substitute for a new capture. Full
combined validation is recorded in `build/flat-pixel-main-validation.log`.

## 45. Exterior hull vectors contradict the image, 2026-09-25

Epic `edvr_gfx_20260925_061613.log` matches `a2625ca0`, build `6AB66598`. F10
at 06:18:04.028 completes frames 35876, 35891, 35906 and 35921 without capture
failures, totaling 225,792,000 bytes. Session directory:
`edvr_logs/flat_pixels/20260925_121804_028_21896_1`. All samples are DLSS K,
1920x1080 to 2560x1440, with live jitter and reset=false. Runtime history
continues across the capture; there are no unknown pairs or backend failures.

Sean identifies the edge lines of the ship outside the cockpit, rather than the
HUD, as the main remaining shimmer. The input rejection rate is 0.615-0.635%;
the reconstructed output footprint is 0.940-0.959%. All bright pixels (RGB
channels at least 180) have zero rejection in all four samples. The white
exterior hull regions match raw DLSS through the finish pass.

Ruled out: the final rejection replacement causes the captured white hull seam
shimmer, because those bright hull pixels are not rejected and raw/final output
agrees there. The safeguard still changes some darker console/radar pixels and
must not be disabled on this evidence.

The offline mask labels 15/42/64/0 raw/final differences as accepted. Every one
is exactly on a horizontal footprint rounding boundary and is covered by
advancing qx one texel. This is consistent with CPU/GPU arithmetic rounding,
not evidence of widespread unmasked overwrites. Keep this analysis limitation
distinct from the large hull-vector defect.

At input ROI x75..269/y790..839, the left hull's median motion is
(+134.875,-47.969) pixels in frame 35876 and (+146.75,-52.156) in frame 35921.
The right hull at x1660..1799/y810..869 gives (-128.5,-49.313) and
(-139.75,-53.625). Both regions have zero rejection. The distant central scene
is around (+0.36,-0.05) pixels. All stored motion/depth values are finite.

The hull's visible silhouette does not support those large per-frame vectors:
left edge positions at six columns are y745,747,746,745 across the four
samples; sampled right edges likewise move only 1-2 pixels across 45 frames.
The vector field contracts toward the image center, consistent with applying
forward camera translation to the player's ship without canceling the ship's
translation. HUD speed is 62 and a roughly 0.67 m/frame translation would be
consistent with that speed at about 90 Hz. This is a hypothesis about the
source, not proof of which engine-record branch produced the vectors.

The matched capture has no engine-slot/record attribution. Shared DLSS uses
low-resolution, unjittered, reversed-depth motion with unit scale; subpixel
jitter cannot explain the magnitude. Do not compensate with sharpening, longer
jitter sequences, preset changes or SS1/DLAA before fixing ownership.

Sean notes the camera moves within the cockpit during high-G maneuvers and that
the menu ship also shimmers. Correct motion must compose object motion with the
actual current/previous camera, preserving this relative camera movement.
Neither zeroing hull vectors nor copying VR's headset delta can represent that
flat camera. VR's existing near-ship/world split explains why its camera
fallback differs; use the shared engine reprojection where its ownership is
established. Do not generalize this flight defect to the menu without a matched
menu capture.

Frame-level engine availability is insufficient: source views are given
1128/1128 times in this capture, but only keyed draw pairs write the slot
target. Projection qualification includes additional pairs that do not emit
slots. Flat `engineBefore` can therefore select camera reprojection for an
unowned pixel, an unmarked record or an unchanged record whose engine math
cannot be reconstructed; a joined record can also produce the observed vectors.
Save the slot target, pool records, source scene buffers and flat
current/previous camera rows together to distinguish these cases before a fix.

The next F10 capture extends the existing bounded readback rather than adding a
rendering workaround. It must attribute accepted hull vectors to their source
and retain raw rows for offline camera/object reprojection. Validate high-G
relative camera movement in the math tests. Capture the main-menu ship with the
same diagnostic, because flight evidence does not determine the menu's motion
path.

Sean also requested automatic flat menu naming: the saved `dlss` choice reads
DLAA when both qualified render dimensions meet or exceed the output, and DLSS
below native scale. Apply the same display name to the preset row and refresh
an open panel when the qualified scale changes. Do not change the saved mode,
cycling order or backend settings. Swapchain image rotation alone does not
invalidate the last observed scale; resize/device teardown does.

Schema 2 retains the six original textures and adds the engine slot texture,
pool and current/previous scene buffers when present, their view offsets and
counts, and both flat camera snapshots. Resource presence is explicit; the 384
MiB cap covers all bytes. Readback still occurs only after F10, without a new
rendering pass or motion correction. The analyzer remains compatible with the
first capture format and adds `--roi X Y WIDTH HEIGHT` in input pixels. Its
engine replay reports sampled branch counts, records used, rejection
disagreements and emitted-versus-replayed motion error. Large regions use a
bounded regular sample grid; those counts are not whole-region totals.

The replay follows the shared rigid-record math, including relative camera
movement. Numerical comparison uses an explicit diagnostic tolerance rather
than claiming bit-exact GPU arithmetic. Prepared depth cannot recover an
original nonfinite/out-of-range depth that the shader sanitized, so ambiguous
rejection cases remain labeled rather than inferred away.

Targeted capture compilation and WARP validation pass in
`build/flat-pixel-engine-targeted.log`. The real producer fixture verifies all
extra bytes, a nonzero pool-view offset, source mutation after the queued copy
and absent engine resources. The updated analyzer verifies that fixture and
still reads all four original flight samples unchanged. Its math tests cover
object/camera translation together, relative camera displacement and object
rotation. Full combined validation is recorded in
`build/flat-motion-source-validation.log`.

## 46. Exact motion attribution: missing flight hull and stale menu slots

2026-09-25: Epic log `edvr_gfx_20260925_064529.log` matches `3261e6e2`, version
`v0.18.0-rc.1-45-g3261e6e2`, build `6AB66C62`. Both manual captures complete
all four samples, ten resources per sample, no failures, and complete engine
inputs. Both use preset K, 1920x1080 input to 2560x1440 output (0.75 SS),
active jitter and continuing history. No headset/runtime is involved. The log
identifies an NVIDIA GeForce RTX 5090; the installed `nvngx_dlss.dll`
file/product version is `310,9,1,0`. Driver version is not recorded by this
log.

Flight session `20260925_124734_750_23404_2`, frames 32260/32275/32290/32305:
the left hull region x75..224/y730..779 and right x1670..1819/y880..929 are
each exactly 7,500 camera-fallback pixels in every frame. Their slots contain
the clear sentinel `(-1,0)`. The offline camera reprojection matches every
emitted vector within 0.28 pixels and reports zero rejection disagreements.
First-frame median vectors are (+341.25,-92.44) and (-314,-138.4) pixels per
frame. The camera origin changes by about (-1.17,+1.67,+0.80) metres that
frame. These visible ownship surfaces receive world-camera translation without
object cancellation. This rules out incorrect joined-record arithmetic as the
source of their motion: no record was selected. The camera-relative motion
needed for high-G sway still has to come from the actual ship draw transform.

The first flight frame has 2,058,708 cleared slot pixels and 14,892 with code
111. All code-111 pixels fail the depth match: absolute differences range
8.29e-7..2.41e-4, median 1.83e-5, beyond D24 quantization. Their slot depth is
usually nearer than final depth. A depth-tested draw with depth writes off can
produce exactly this pattern; the existing engine WARP test covers it. Do not
conclude from this sign alone that another draw later covered those pixels.

Main-menu session `20260925_124610_101_23404_1`, frames
24903/24918/24933/24948, is visually confirmed as the ship in the hangar. About
55.3% of input pixels reject history in each frame. White upper hull at
(840,330,240,120) is entirely camera fallback; its first-frame median motion is
only (-0.0096,-0.0115) pixels because the camera is nearly stationary. The
sampled canopy is 18,884/18,900 stale-depth pixels; the outer wing is
42,810/43,200. Rejected regions emit zero motion and use current-frame colour
in the finish pass, explaining their weak response when AA is switched.

In menu frame 24903 one static joined record, slot 60/code 121, covers
1,231,140 pixels in the slot texture, of which 1,147,219 fail final depth. The
median signed final-depth minus slot-depth difference is +0.000314, consistent
with nearer visible geometry replacing a farther slotted surface. The
subsequent samples show the same pattern with slots 56/56/57. This is not proof
of which shader draws the ship. Offline replay matches motion and rejection,
with zero rejection disagreements; D24 rounding explains none of the stale
pixels exactly.

- Ruled out: engine joined-record math causes the sampled flight hull vectors,
  because all 60,000 inspected hull pixels select the cleared-slot camera path.
- Ruled out: D24 rounding explains these stale regions, because measured depth
  differences are much larger and quantizing the slot depth does not match.
- Ruled out: the menu and flight have one uniform rejection problem, because
  the flight white hull is accepted while the menu canopy/wing mostly reject.

The named source requires the same scene-depth resource at the final handoff;
this excludes an unrelated slot/depth pointer but cannot prove every visible
surface wrote that depth or had an engine slot. Naming starts at the first
supported pool draw, which may follow nonpool ship draws. The next diagnostic
therefore records bounded native MRT0..3 windows before and after each
scene-sized draw, raw VS b0/b1/b2 snapshots and actual render/IA state across
two consecutive frames. It starts from the previous qualified render extent,
includes earlier and other-depth draws, and reports partial coverage
explicitly. It does not change shader code, blend state or motion. No
speculative motion correction or depth-match tolerance is justified by this
run.

The F10 implementation saves `flat_draw_pixels/<session>/frame_N.json` with
packed native pixel and constant-buffer blobs. It samples eight normalized
points, 16x16 pixels each, on MRT0..3 before and after up to 512 draws per
frame. Two consecutive qualified frames are requested; unsupported formats,
missing inputs, interrupted runs and resource limits are explicit. Shader
bytecode is retained through the existing flat shader cache. These passive
copies do not change shader code or render state. Reads are asynchronous and
bounded; no capture work runs outside an armed diagnostic.

`tools/flat_draw_pixels.py` reports the chronological byte changes per sampled
window, actual draw arguments/state and raw constant-buffer availability. A
changed window proves that a draw affected those samples, not that it owns all
final visible pixels. Ordinals and resource pointers alone do not prove
cross-frame object identity. Dry-run writes nothing. The build checks the
tool's malformed/partial-data tests and its compatibility with the WARP
producer fixture. Full validation: `build/flat-draw-owner-validation.log`.

## 47. Menu draw evidence and cockpit-loading crash, 2026-09-25

Epic log `edvr_gfx_20260925_072306.log` matches installed `2dc6aabf`, version
`v0.18.0-rc.1-46-g2dc6aabf`, build `6AB67508`. Sean captured the menu with F10,
then the game crashed while loading into the cockpit. Investigate the crash
before requesting another flight or changing motion math. Sean confirms this is
the first occurrence of this cockpit-loading crash.

F10 armed at 07:23:45.473. Draw capture session `20260925_132345_473_24612_1`
saved frames 26431 and 26432, 353 draws each, by 07:23:45.699. Each reports
partial coverage because four late draws (q322-325) use unsupported native
format 53 on RT1: 64 refused windows per frame. The relevant scene MRT0/1/2
windows are available. Standard flat pixel capture saved all ten resources in
four samples, completing at 07:23:46.244. The draw queue and its resource
references were retired before loading.

Upper-roof window 2 changes in q153, VS `66DE2CADB1F4AE6B` / PS
`864F1F949851B8DE`, on all 256 pixels. Left-wing window 5 changes in q142, VS
`DE545DC8EE4FBB87` / PS `E46E3E4832B2FDB0` (201 pixels), then q153 (256
pixels). Both are existing engine-motion families. These sampled hull windows
nevertheless have cleared final slots; the other sampled ship windows contain
stale slot code 79. The menu's camera motion is small here, so this capture
does not establish the high-G flight transform.

The first declared engine-family draw is q2. Draws q142/q153 have the same
1920x1080 viewport, native RT0 format 23, scene DSV/depth resource, VS b1
resource and exact 96 camera bytes at rows 270-275 as q2 and the matched
flat-pixel camera. Their b2 buffers are unchanged material-like data across the
two frames, not an identified object transform. Captured bindings precede
EDVR's shader/MRT substitution; they do not prove these calls wrote MRT6.

- Ruled out: these menu hull draws require a new shader-family inventory,
  because both pairs are already declared engine-motion families.
- Ruled out: different captured depth, viewport or scene-camera rows exclude
  q142/q153 from naming, because all match the earlier q2 source candidate.

Windows recorded exception `c0000005` at `EliteDangerous64.exe+0x540598` for
PID 24612. The retained local dump is
`%LOCALAPPDATA%\CrashDumps\EliteDangerous64.exe.24612.dmp` (106,441,397 bytes).
The faulting instruction, `mov [r8+0x10],rdx`, writes to address `0x10` because
r8 is null. Surrounding instructions unlink a linked-list entry; the entry's
link at offset 0x18 is zero. This identifies the failure site, not who
invalidated the entry. A game-module fault alone does not exonerate EDVR or
establish a game-only bug.

PE exception-table unwind confirms 13 game frames, ending at the kernel32
thread entry. Callers `+0x55b528`, `+0x54c88f` and `+0x5c2902` dispatch a
16-byte small allocation from colon-delimited string processing. Higher game
frames are `+0x48e32bd`, `+0x48ea951`, `+0x48ece94`, `+0x48ec499`,
`+0x48d7d3f`, `+0x48d6f25`, `+0x5c42f1`, `+0x54f176`, `+0x55cba6`. There is no
EDVR or graphics module on this active call chain. The selective dump omits the
damaged node's memory, so its prior writer cannot be recovered from this dump.
Matching game/proxy binaries are preserved locally under
`build/crash_24612_artifacts`; the dump is also retained under `build`. This
build has no PDB/CodeView record, so no matching proxy PDB was available.

The last log at 07:24:46 has no backend failure, context change, allocation
reset or full temporal reset. New draw-capture work finished roughly a minute
earlier. Its inactive path does not retain captured GPU resources. The source
audit found balanced COM ownership and bounded atlas/constant-buffer reads;
this does not rule out earlier corruption or a timing effect. Older WER events
at game offset `0x4d78c51` are a different signature and must not be treated as
evidence for this loading crash. Dumps and captured game data stay local; only
the investigation conclusions are publishable.

Next check: keep Epic `2dc6aabf` and its settings unchanged, restart and load
directly into the cockpit without F10 at the menu. A repeat would rule out a
menu F10 capture as a necessary trigger; success would not prove capture
causality. If cockpit loading succeeds, take the intended flight F10 only after
the cockpit has stabilized. No speculative allocator or motion fix is justified
by this dump.

## 48. Successful cockpit retry and motion-target lifetime, 2026-09-25

Sean loaded directly into the cockpit on unchanged Epic `2dc6aabf`, then
pressed F10 successfully. Log `edvr_gfx_20260925_074058.log` verifies build
`6AB67508`. Standard session `20260925_134252_260_35872_1` completed four
ten-resource samples (frames 33211, 33226, 33241, 33256), zero failures. Draw
session `20260925_134252_262_35872_1` saved frames 33211 and 33212, 124 draws
each. This successful retry does not establish the earlier crash's cause or
rule out an intermittent capture/overlay interaction.

Sean noted Epic's injected overlay. The earlier crash dump confirms
`EOSOVH-Win64-Shipping.dll` and `EOSSDK-Win64-Shipping.dll` were loaded.
Neither appears in the proven faulting call chain. Overlay presence is
established; overlay responsibility is not. No overlay settings were changed.

The static binding trace exposes a concrete hypothesis for the missing or stale
slots. `FlatRuntimeDrawScope` restores the game's original output targets after
every producer draw, under `FlatComputeInternalScope`, which deliberately
bypasses game binding-generation tracking. However,
`engineVelocityAfterFlatDraw` restores shaders/blend and clears `DrawCache`
without invalidating the source eye's remembered MRT6 binding. On another draw
with unchanged game output bindings, `slowPath` can see matching
`Eye::rtvGen/dsvGen` and skip `bindTarget`, although MRT6 was removed by the
previous flat scope. This predicts first-draw slots with absent/stale slots on
later geometry, without a shader-family or camera-math failure.

Discriminator: use the existing production-code WARP rig to draw twice with the
flat scope's restore sequence and unchanged binding generations. Inspect actual
MRT6 and its pixels on the second draw; establish failure before a fix. A
correction must invalidate only the flat binding lifetime, retain first-draw
slot pixels and frame snapshots, and leave VR's binding reuse intact. No extra
flight is needed to test this API-state sequence.

The cockpit draw capture corroborates this failure. At frame 33211, right wing
window 7 is changed in all 256 pixels of MRT0/1/2 by keyed q42
(`66DE2CADB1F4AE6B` / `864F1F949851B8DE`), but all final slots remain -1; the
center takes approximately (-303, -104) pixels of camera motion. After an
output-target switch at q54, keyed q55 changes 17 pixels in window 1; the final
code-123 slot mask matches those 17 pixels exactly, although later depth
disagreement rejects them. This is consistent with a first draw after target
rebinding writing slots and later same-pass geometry not writing.

A separate remaining surface is q6/q11, VS `BFE51414CC3024B4` / PS
`DB79AE788E049DFD`: the known non-pool cockpit shell. Its projection recipe
already uses VS b1 row 270, but it does not export engine-pool ownership in VR
or flat. q6 changes all 256 pixels in windows 1 and 4, with cleared final
slots. Fixing MRT6 reattachment alone cannot give this family object motion.
Its scene camera matches the selected camera, and the captured b2 buffers are
unchanged material/config data; an object transform is not established.
Preserve this distinction when evaluating the next flight.

The draw capture's partial flag comprises 96 unsupported windows per frame:
q61-62 RT0 native format 60 (32 copies), q63-66 RT1 native format 53 (64
copies). There is no draw overflow; the ship windows above are available.

The exact flat source-path regression fails against the unchanged production
source at `second same-pass draw rebound MRT6` (`build/flat-mrt6-red.log`). The
correction adds an explicit source-eye binding-stale flag when
`engineVelocityAfterFlatDraw` is called. The next eligible draw validates depth
and reattaches MRT6 even if game output-binding generations are equal. It
preserves slot contents and snapshots and does not invalidate VR eye bindings.
No new capture or per-draw diagnostic is needed.

The same production-code WARP suite then passes 1,022 checks
(`build/flat-mrt6-green.log`), including real MRT6 resource identity, first
draw slot-5 pixels preserved byte-for-byte and second draw slot-9 pixels
written. The regression uses `engineVelocityNoteSource` and
`engineVelocityBeforeDraw(..., false)`, with a raw output restore between draws
and unchanged game binding generations. Full validation is recorded in
`build/flat-mrt6-validation.log`; visual improvement still needs qualification.

## 49. Binding fix qualified; local shell history loss remains, 2026-09-25

Epic `edvr_gfx_20260925_075739.log` verifies `65db2993`, build `6AB67D7F`,
version `v0.18.0-rc.1-48-g65db2993`. Sean reports cockpit Off/DLSS A/B is much
better. The menu still consistently shimmers (screenshots 07:58:34 and
07:58:41). A sporadic apparent history reset mainly affects the white panels
and exterior edge lines, rather than the whole 3D scene (screenshot 08:00:36).

Menu session `20260925_135824_887_4516_1`, frames 26933/26948/26963/26978, has
no reset in any sample. Motion-slot ownership is decisively restored: frame
26933 roof P2, left wing P5, canopy roof P6 and right wing P7 each have 256/256
exact-depth slots; canopy P3 has 230/256 and edge P4 245/256. The previous menu
run had entirely empty P2/P5 and stale P3/P4/P6/P7. Across the four new
samples, P5/P6/P7 remain exact on all pixels; P2 ranges 240-256, P3 229-231 and
P4 242-245. Global rejection falls from about 55.3% to 24.3%, now concentrated
in other surfaces and localized edge/overlay disagreements. Accepted ship
motion is small, approximately 0.02-0.05 pixels per frame. Remaining menu
shimmer is not explained by the former missing-target bug.

Cockpit session `20260925_140022_707_4516_2`, frames 37317/37332/37347/37362,
also records final backend `reset:false` throughout. White panel P1 (1747,907)
has cleared slots on all 256 pixels in all four samples, rejection zero, and
camera-fallback median motion approximately (-336,-156), (-314,-146),
(-337,-157), (-334,-156) pixels. Draws q5/q6/q9, VS `BFE51414CC3024B4` / PS
`DB79AE788E049DFD`, change 7+198+51 pixels in that window. This is the known
non-pool cockpit shell, not a new engine-pool family. In contrast, adjacent
wing P7 is changed by keyed q42 `66DE2CADB1F4AE6B` / `864F1F949851B8DE`: all
256 pixels have slot code 11, 255-256 exact depth, 0-1 rejected pixels and
motion approximately zero. The local shell's erroneous motion is consistent
with losing smoothing while the supported wing retains it. P4 is sky/edge above
the wing in this capture; do not mislabel its fallback motion as another white
panel measurement.

Two genuine camera-cut resets are counted in the flight. The second occurs
between 08:00:20.201 and 08:00:25.205 but not in the four saved F10 frames.
From 08:00:30 through 08:00:40 the reset counters remain unchanged; no full
reset is logged around the third screenshot's time. The F8 mode cycle at
08:00:11-13 accounts for separate expected invalidation/reinitialization. The
resolver's independent camera-cut heuristic compares each raw camera position
component against a fixed 50-unit step; aggregate counters omit the exact frame
and input delta. Current evidence cannot distinguish an earlier true camera
jump, ordinary travel during a long frame, or coordinate rebasing. Do not relax
the guard without the event's actual inputs.

- Ruled out: the new cockpit panel samples show global DLSS resets, because all
  four manifests store the final internal reset decision as false.
- Ruled out: the remaining sampled menu roof/wing shimmer is simply absent MRT6
  ownership, because the same windows now have exact-depth slots.
- Confirmed: the sampled white cockpit shell still uses world-camera motion
  while adjacent keyed geometry receives near-zero object motion.

Captured bytecode corrects the historical classification: BFE is a pool shader.
It reads structured t33 with stride 336, indexed by `INSTANCEANDMODELDATAINDEX`
v0.x, loads root scale/quaternion and translation at offsets 0/16, subtracts
cb1[275] and projects with cb1[270..273]. It exports the index in
`__USER_MATERIALMODULATION_DATAID` o0.y; DB79 consumes the matching v0.xyz
signature and writes MRT0..3. Existing input derivation and pixel-shader
patching accept the pair. The local WARP corpus checks 40,960 original
G-buffer/depth texels with zero differences and 8,192 MRT6 texels with zero
errors. No proprietary shader bytes are committed.

This is a missing exact pair declaration, not evidence for a new motion
approximation. The normal producer/consumer pool identity and previous-pose
marker checks still apply. BFE can optionally read a 48-byte t38 bone palette
before applying the root pose. Root motion alone does not establish previous
bone deformation; the next capture must distinguish joined, masked and
camera-fallback records for this actual shell. New eligibility must remain
flat-only while VR is unqualified.

- Ruled out: BFE is intrinsically non-pool, because its captured structured t33
  load, record layout and exported index match the existing pool path.

Menu evidence also establishes real jitter: the P3 diagonal ownership edge
moves approximately +0.688/-0.375/+0.563 pixels between the four samples,
versus jitter projected onto that edge of +0.583/-0.410/+0.736 pixels. The sign
and magnitude agree within small scene motion and raster quantization. Do not
revive a missing-jitter hypothesis for this edge.

Its 26 rejected pixels are exactly the color-change mask of q309, VS
`BBE58E40FE88EC80` / PS `DB3E8D20CF53FBC0`, with depth writes disabled,
GREATER_EQUAL testing and zero rasterizer bias. Stored slot depth is nearer
than unchanged DSV depth by about 1.61e-6 in reversed Z. This is a biased
overlay writing ownership without updating scene depth, not numerical noise. P4
has ten of eleven rejects matching q269/q309 of the same family/state; the last
pixel's depth difference suggests q269 but color bytes do not prove that
pixel's writer. Do not relax depth equality to conceal this disagreement.

Implemented: declare only BFE/DB79 as a flat-only family. Runtime family
eligibility covers shader remembering, draw preparation and source admission,
so VR's prior first-draw/snapshot behavior stays unchanged. Shared input
derivation, slot patching, pose reconstruction and high-G camera terms are used
without a special motion approximation. The targeted engine WARP suite passes
1,025 checks, including actual runtime-profile eligibility calls. The real BFE
pair's corpus checks pass; the entire optional Epic corpus run cannot pass
because the dump lacks an unrelated station shader. Do not call that whole
corpus green.

Also implemented: bounded successful-reset events record exact frame, mode, all
reset causes, elapsed milliseconds, current/previous camera origins, origin
delta, largest matrix delta and jitter. Separate 32-event budgets for ordinary
resets and camera cuts survive renderer reinitialization. The resolver rig
checks event contents, continuation silence and both budgets; history decisions
are unchanged. Full build log: `build/flat-shell-motion-validation.log`.

Next Epic flight: compare the same white cockpit panel and take F10 during
normal camera/ship movement. Verify new BFE substitution, stable source views,
valid matched slots, record kind/previous pose and corrected panel motion; do
not assume skinning history is available. Inspect precise reset events if the
symptom recurs. Menu overlay ownership remains unresolved: preserving
underlying slots or inventing an unbiased depth cannot establish decal motion
unless attachment/record identity is proved. No depth tolerance or overlay
policy was changed in this build.

## 50. Shell motion qualified under movement, 2026-09-25

Epic log `edvr_gfx_20260925_082217.log` verifies `4325252c`, build `6AB68316`,
version `v0.18.0-rc.1-49-g4325252c`. Sean reports mostly stable panels while
stationary and remaining edge artifacts under motion (screenshot 08:25:10).
This run uses 2560x1440 render and output, unlike earlier 1920x1080 captures.
Recompute all normalized probe coordinates before comparing regions.

The two pixel sessions are `20260925_142513_725_21064_1` (frames
62650/62665/62680) and `20260925_142519_933_21064_2` (63186/63201/63216). Each
saves three complete samples and stops at the existing byte cap before the
fourth; zero readback failures. Matching draw captures retain two frames with
224 draws each. The second draw directory's millisecond component is 934, while
the second standard-pixel directory uses 933.

The new reset events establish no reset near either capture or the screenshot:
after the startup projection-preparation event at frame 56846/08:24:09, history
continues for more than 6,500 frames. Camera cuts and backend failures remain
zero. All six sampled manifests record final backend reset false. BFE is live
with 34,788 substitutions in the last summary window. Source views are given on
all 2,676 frames, with no source invalidations or naming declines.

At stationary P1 (2329,1209) and P4 (2099,964), all 256 pixels in each window
across all three frames have exact-depth slot code 165, record 82, with a
validated joined previous pose and zero rejection. These pixels are changed by
BFE/DB79 draws q5/q6/q9. Its record word 0 is zero: the optional t38 skinning
branch is disabled. Rigid root reconstruction is applicable here, and replay
matches emitted motion within roughly 0.0002-0.0003 pixels. The small correct
vector replaces the formerly large camera-only vector.

In the moving capture, P4 retains joined records 82/81 on 203/199/233 pixels in
the three samples. Their center motion is approximately (+0.106,-0.028),
(-0.068,-0.159), (-0.255,+0.541) pixels, versus raw camera fallback around
(+19..23,+7..9) pixels. This preserves real nonzero relative motion. P1 moves
onto another panel with exact-depth code 13 on all 256 pixels and motion
approximately (+0.288,+0.029), (-0.243,-0.508), (-0.379,+0.553). P4 has
45/47/23 rejected pixels and P7 has 21/21/5: investigate those localized edges
rather than undoing the now-qualified shell pose path.

- Ruled out: missing BFE ownership/previous pose causes these remaining moving
  artifacts, because its rigid records now join and replay correctly.
- Ruled out: global history resets caused either sampled sequence, because both
  precise events and renderer counters show uninterrupted history.

Offline reader correction: `flat_draw_pixels.py` formerly calculated normalized
centers in Python binary64, which gives int(0.7*1440)=1007. The C++ producer
uses float32 and obtains 1008. The reader now reproduces float32 literal and
multiplication rounding exactly, without widening coordinate validation. Its
self-test includes 2560x1440 and rejects the malformed y=1007 witness. Both new
sessions validate via dry-run without changing capture files.

The first moving P4 window has 45 rejected pixels. Draw q35, DE54/91F8, changes
47 colour pixels, including all 45 rejected pixels. It writes nearer depth with
GREATER_EQUAL and depth writes enabled; final scene depth exceeds the stored
slot depth by 0.000267-0.000524. Earlier BFE draws owned the slot, but PS91 is
refused by the patcher and cannot replace it. Later colour passes also touch
the region; q35 is the depth-writing geometry responsible for the ownership
mismatch, not the last colour writer. This is not a tolerance issue.

Captured DE54 exports SV_Position at VS o4, while PS91 uses PS v4.x for
rasterizer-generated SV_IsFrontFace and declares no SV_Position. The patcher
incorrectly requires the PS position register to match the VS output register.
A candidate correction allocates a free PS register for rasterizer position
without changing front-face input. The existing corpus identity fixture uses a
synthetic VS derived from the patched PS signature: its unchanged G-buffer
comparison alone cannot qualify real DE54-to-PS91 linkage. An actual captured
VS draw is required before enabling the pair.

P7 is a separate overlap case: 16 of its 21 rejected pixels coincide exactly
with depth-write-off BBE/DB3E draw q150, with final-minus-slot depth around
-0.0045. Five others have a smaller negative discrepancy and are not explained
by that draw. Do not generalize either case to all overlays or relax depth
ownership. Final-image repairs stay entirely within the predicted 2x2 reject
footprint; stationary P4 and moving P1 have no repair. Global history remains
valid while the affected edge locally falls back to current colour.

Before the next build, Sean requested another main merge. Fetched main
`b969a4e5` brings the GPU feature census, build concurrency lock and FSR 3.1
menu label. The only textual conflict is the native menu rig's includes and
stubs; retain both flat-runtime support and GPU census stubs. The combined
source must pass the normal full build before commit and receipt-guarded
DLL-only promotion before installation.

Offline qualification now draws the actual captured DE54 VS into stock and
patched PS91 on WARP, using controlled packed vertices, a rigid t33 record and
scene constants. Both windings cover 1,352 pixels (2,704 total); reversing the
winding changes the stock front-face-dependent colour. Across 40,960 game
target/depth texels, stock and patched outputs are byte-identical. All 2,704
covered motion slots contain exact code 11 and bit-identical fragment depth,
with zero bad slots. Initial zero coverage was a fixture error: the compact
position mode flag belongs in packed vertex A.z, not A.w. No runtime change was
admitted from the failed fixture.

The patcher now uses an existing PS SV_Position input where present, otherwise
allocates a free input if the VS output register is occupied in the PS. It
leaves PS91's front-face input untouched. Only DE54/91F8 is newly enabled, only
in flat mode; A607 remains unqualified. Profile tests retain existing DE54 VR
pairs and exclude PS91 from both explicit and legacy VR. The synthetic
front-face collision test runs in the normal build, and the optional
`engine_velocity_test.exe --real-link <edvr_logs>` gate repeats the actual
captured pair without needing unrelated station shader dumps. No game shader
bytecode is checked in.

The first combined build caught stale fixture assumptions in
`flat_temporal_test`: its camera-update helper touched only the formerly
supported records, leaving newly supported PS91 on the old camera; its
no-supported-source case likewise left PS91 enabled. Update the PS91 fixture
camera with the scene and use the still-unqualified A607 in that negative case.
Retain production camera-ambiguity rejection and all expected refusal reasons.
The captured source split is now 22 supported draws and four unsupported leg
draws under the current table.

The combined full build passed and was committed as `24500664`. Its clean
`--dll-only` promotion exposed an imported build-guard bug: argument parsing
uses SHIFT, so the later `%~f0` expands to `--dll-only` rather than the script.
The guard and test-runner invocation now use the absolute `%ROOT%\build.bat`
path retained before parsing; `--jobs 8` exposed the second affected call. This
repair requires another full build and clean promotion; no unvalidated DLL is
installed. The independent lock-acquisition race is not changed in this
rendering task; only one top-level build is run at a time.

Next Epic capture: move across the same thin white-panel seam. Look for PS91
substitutions and matching depth/ownership at the seam formerly covered by q35;
verify joined previous pose and uninterrupted backend history. The
depth-write-off BBE menu/outer-panel issue and five unresolved P7 pixels remain
separate. Do not promise all ship-edge shimmer is resolved by this change.

## 51. PS91 ownership qualified in the next Epic capture, 2026-09-25

Epic `edvr_gfx_20260925_090601.log` verifies installed `aaa3d020`, version
`v0.18.0-rc.1-56-gaaa3d020`, build `6AB68DA7`, linked 15:05:11 UTC. Render and
output remain 2560x1440. Pixel session `20260925_150817_933_36344_1` contains
frames 32964/32979/32994: three complete samples, zero failures, then the byte
cap. Draw session `20260925_150817_934_36344_1` contains frames 32964/32965,
124 draws each; partial copies are explicit rather than assumed missing draws.

At point 1 (x=2321..2336, y=1201..1216), q35 DE54/91F8 changes 45 pixels in
frame 32964. Those exact 45 pixels now carry code 117 (record 58), all with
bit-identical slot/scene depth and zero rejection. The other 211 pixels carry
code 7 with exact depth. Across all three samples, P1 has 256/256 valid joined
pixels and zero rejection. The second draw frame's q35 changes 42 pixels. The
producer reports PS91 success at 09:08:17.944. This proves live ownership for
the formerly unpatched geometry, beyond the offline WARP gate.

The camera view differs from the previous capture: q35 now touches P1 rather
than P4. Do not present the samples as a spatially matched before/after image.
P4 is all 256 joined BFE pixels with no rejection in each saved frame. P7
retains 25/27/26 rejected pixels; in the first frame all 25 exactly match the
changed-pixel mask of q62 BBE/DB3E. That draw has depth writes disabled; final
scene depth minus stored slot depth is -0.0003177 to -0.0003011. It is the
previously identified overlay ownership issue, separate from PS91.

P1 and P4 have zero final-image repair pixels in all samples. P7's predicted
2x2 finish masks cover 45/46/44 pixels, exactly matching the raw-to-final
changes, with none outside the masks. The remaining local history loss is
therefore measured independently of the successful PS91 repair.

All three saved samples retain backend history. There is one real earlier
interruption: frame 32810 at 09:08:16.194 refuses projection preparation after
80 jittered draws, uses spatial fallback, then frame 32811 resets history. The
first F10 arms at 09:08:17.933. Cumulative diagnostics include one missing full
write and one plan failure; a cold buffer was queued, then went stale. The
failure occurred before the timed per-draw audit, so the log does not name the
failing shader/buffer. Do not turn these counters into a guessed fix.
Camera-cut and backend-failure counters remain zero.

- Ruled out: PS91 remains unpatched in this run, because its exact changed
  pixels now carry valid record 58 ownership and joined history.
- Ruled out: a global reset caused the saved P7 rejection, because all saved
  samples retain history and its first-frame rejects match the overlay mask.

Sean's visual assessment was requested separately. Installed build remains
`aaa3d020`; a documentation-only follow-up does not change the game DLL.

## 52. Joint overlay and jitter-failure diagnostics, 2026-09-25

Sean authorized the next diagnostic build. The existing draw capture has native
MRT0-3 and constant buffers, but no motion slot immediately before and after a
draw. Shared VB/IB addresses do not prove attachment: current q42 and q62 have
different instance starts. Retaining the underlying slot without proving record
identity could assign another object's motion to the overlay.

Extend the F10-only capture to record actual MRT6 slot/depth and matching DSV
windows around the sampled draw. The original capture still sees game shader
identities and unmodified constants. A second pre-draw hook runs after the
engine producer successfully substitutes the shaders and attaches MRT6; the
post-draw sample runs before projection, shaders and targets are restored. An
existing game RTV6, refused substitution or unsupported resource remains
explicitly unavailable rather than being mistaken for engine ownership.

For the exact underlying 66DE/864F and overlay BBE/DB3E pairs, capture bounded
t33 contents and SRV range/stride as well. Matching before/after pixel codes
and unchanged relevant record bytes distinguish an attached surface detail from
a reused pool slot. Copies are asynchronous, capped and restricted to the two
F10 draw frames. Normal rendering does not acquire these snapshots. Each pool
snapshot is limited to 4 MiB, eight per family and 16 per frame, within the
shared 256 MiB capture cap. The analyzed menu pool has 2048 records of 336
bytes each (688128 bytes). Auxiliary capture status is separate from the
existing color chronology; unavailable motion evidence stays explicit.

The latest captured frames each contain three underlay draws (q40/42/44) and
five overlay draws (q59-63), so the family cap includes the relevant q42/q62
pair. At 2560x1440, the observed formats and pool size imply roughly 212 MiB
for both frames, including depth mirrors, packed windows and constant buffers.

Depth-stencil region copies have an API restriction: only whole subresources
may be copied directly. Use a reusable full-size mirror without depth binding,
then copy its probe windows into the packed staging resources. Charge mirror
memory to the existing cap. This avoids depending on an invalid partial-depth
copy even if a particular driver accepts it. See the [D3D11 copy
contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copysubresourceregion).
The live resource uses `R32G8X24_TYPELESS`, eight bytes per pixel, with a
`D32_FLOAT_S8X24_UINT` view. Compare its first float's exact bits to slot
depth; do not assume that the source resource is four-byte `R32_TYPELESS`.

Projection preflight/prepare failures now retain failure-only metadata: exact
return branch, shader/request context, buffer identity/range/generation,
complete-shadow availability, mutation serial, map/cold-readback/private-buffer
state and cached plan/topology availability. The runtime logs this before
failing the phase, independently of F10. Limit to one event per frame, 32
events after jittered draws and eight before any draw; resets do not replenish
these process-wide budgets. Successful preparation clears stale diagnostics.
This distinguishes new topology, missing writes and binding changes without
loosening any admission rule or forcing history retention.

Validation must cover actual before/after motion and depth with a
depth-write-off overlay, unchanged game outputs, parser compatibility with old
captures, byte caps and dry-run no-write behavior. Projection tests must name
the exact failing branch and clear its context on subsequent success. Run the
full build, commit the same source, promote the clean DLLs, then install and
verify Epic while preserving the INI. This build gathers evidence; it does not
claim to eliminate the remaining overlay shimmer yet.

The native-format WARP fixture passes: the underlay writes slot `(7, 0.5)` and
DSV `0.5`; the depth-write-off overlay retains code `7`, exports slot depth
`0.25`, and leaves DSV `0.5`. Both draw-time pool snapshots contain the same
four 336-byte records. Capture versus a no-capture replay produces
bit-identical color, MRT6 and depth, with no D3D debug-layer hazards. The
projection rig also distinguishes first-seen live topology, missing full
writes, cold-readback state and changed binding ranges, and clears stale
failure metadata after successful preparation.

## 53. Same-record overlay rejection and live topology refusal, 2026-09-25

Epic `edvr_gfx_20260925_094244.log` verifies `7b88349c`, build `6AB6963A`,
linked 15:41:46 UTC. Both captures use 2560x1440 render/output with DLAA on the
RTX 5090; the installed DLSS runtime's file version is `310.9.1.0` (read
locally). This is the flat D3D11 path; VR runtime/headset are not involved. The
menu draw capture is `20260925_154332_580_13892_1`, frames 36837/36838: 404
draws, 94 motion candidates, 78 pool-cap refusals. The cockpit capture is
`20260925_154457_090_13892_2`, frames 44204/44205: 145 draws, eight motion
candidates, zero auxiliary refusals. Its motion/depth evidence is complete
despite partial color capture for unsupported formats.

Frame 44204's left white panel P0 has exact-depth code 15 (record 7) after q42
66DE/864F. Depth-write-off q78 BBE/DB3E changes exactly 26 color and slot
pixels, retains code 15, and leaves native DSV unchanged. Those 26 previously
exact-depth pixels become exactly the final rejected pixels. P7 has six more
slot changes with the same signature, even though their native color bytes do
not change. Both draws snapshot the same t33 resource and unchanged record 7
(SHA256 `c1d91ed2c591bf3c48e6bbd3686506f7e9f8c62cfde10fdf24fabe063340f46a`).
Frame 44205 repeats 29 changed pixels at P0, again with equal record hashes
between underlay and overlay. No sampled BBE window changes the slot code.
These samples prove same-record attachment locally; eight windows do not prove
that every draw in this family has a matching depth-writing parent.

The independent reset event is frame 42393 at 09:44:36.742: VS
`1F17BF54DB6EE407`, PS `A75C1DB6562B8CA7`, branch `topology-first-seen-live`,
after 79 jittered draws. Its VS b1 source is tracked, promoted, private-ready
and has a complete shadow. The new recipe patches two forward matrices at byte
offsets 656 and 720. A binding-plan cache miss, before the ready source is
validated, refuses the frame; frame 42394 resets history. This is separate from
the captured overlay rejection.

- Ruled out: missing source writes or a pending cold readback caused frame
  42393, because the failure snapshot has a complete shadow, ready private
  buffer and no pending readback; the exact refusal is missing topology.
- Ruled out: a different object record explains the sampled cockpit overlay
  rejection, because pre/post codes and draw-time record bytes are identical.

Implemented an allocation-free live binding-plan path only for already tracked,
shadow-complete, private-ready sources, retaining exact phase, binding, range
and prepared-token validation. A single preallocated plan is retargeted only
after warm setup has proven D3D11.1 support; active scopes prevent retargeting.
The WARP rig passes the captured two-patch recipe and negative shadow, private
buffer, stale-token and binding-range cases. `flat projection live plans`
reports successful retargets for the next flight. Any overlay correction must
preserve strict depth ownership and reject cases without a matching substrate;
do not add a tolerance or suppress all depth-write-off motion exports.

The menu's P3 ship edge shows the same mechanism at q351 BBE/DB3E: 29 pixels in
frame 36837 and 31 in 36838 change color and slot depth, keep the same odd
owner code (191, record 95), and leave native DSV unchanged. The first frame's
changed mask equals final stale-depth and rejection masks exactly. Slot depth
exceeds DSV by 1.6006e-6 to 1.6504e-6. q351's pool snapshot is unavailable, so
the capture cannot prove its record bytes. The 69 BBE draws q292-360 form one
contiguous group. A separate 256-pixel P1 stale region already exists before
these families; its preview lands on dark hangar floor beneath the ship's right
wing, outside the white-panel correction's scope.

Guarded overlay design: retain a separate snapshot of the depth-owning motion
slots before this exact overlay group. A flat-only shader variant preserves
snapshot depth only when its odd owner code equals the current fragment's code.
Missing or different owners retain the normal exported depth and the consumer's
strict rejection. Reuse the snapshot across depth-write-off overlays,
refreshing it after another producer writes slots and on every new frame.
Existing pool replacement and camera checks remain prerequisites. The observed
menu needs one 29.5 MB logical copy at 2560x1440, rather than one copy for each
of its 69 overlay draws; measure actual group/copy counts in the next flight.
VR's producer behavior remains unchanged.

The guarded shader WARP fixture passes 80 matching-owner pixels, 80
different-owner pixels and 80 empty-substrate pixels at a nonzero viewport
origin. Only matching owners select substrate depth; other cases retain
fragment depth. The stock, normal motion-export and guarded shaders produce
bit-identical MRT0-3 colors. A shader already using the borrowed t3 binding is
refused. The actual production draw-hook WARP sequence also passes: two
consecutive overlays reuse one copy, an intervening underlay forces a second,
and the next frame forces a third. Four guarded draws have zero fallbacks; the
original game t3 binding returns after each draw. The engine-motion rig passes
1,114 checks. The real Epic DB3E guarded shader patches and creates on WARP;
the optional whole-corpus run stops later on an unrelated missing shader dump
and must not be described as passing. Full validation and flight qualification
remain separate gates.

## 54. Resolution transition and new projection pairs (2026-09-25)

Verified Epic `edvr_gfx_20260925_101944.log` against `22fe85d2`, build
`6AB69EDB`. Full validation, clean DLL promotion, feature-branch push and Epic
installation passed before this run. The INI hash remained unchanged. Sean
reports no shimmer in the menu and apparently none on the hull. Both F10s saved
three complete final samples without failures: menu
`20260925_162038_975_14456_1` and cockpit `20260925_162151_205_14456_2`. The
overlay summaries have zero state/resource/shader declines, including 4K; menu
has 69 guarded draws per copy. The live-plan correction exercised one
successful retarget.

The 2560x1440 to 3840x2160 transition occurs at 10:23:10.872. At 10:23:25.952,
temporal rendering is healthy: 449/449 frames continue history, zero failed
frames or resets, history-valid=1 and continue-run=1022.

Ruled out: resize permanently losing the DLSS backend, because 4K jitter and
history recover and continue before the later failure; backend-failure remains
zero throughout. Sean changed only fullscreen and resolution, not other
graphics settings. The log does not establish why these permutations appeared.

At 10:23:27.295, frame 41511, three unknown projection pairs use the exact
named/phase scene depth, a 3840x2160 viewport and normal depth range. They fail
coverage after 45 draws were jittered, force spatial fallback for that frame,
then prevent history accumulation on every subsequent frame:

| Draw | VS | PS |
| --- | --- | --- |
| q385 | `71DD9863DCFC0986` | `43E5E6EB67AC751B` |
| q445 | `3530A6FD15EDE145` | `13B224F056C39D85` |
| q447 | `9FFA5D5E79F04873` | `8134D09E3462E904` |

At 10:24:05.698, `19F70CE80DA3242B/C8FBD8A982C0729C` adds another unknown pair
at 4K. At 10:24:26.349, `A52ECB960783BB35/84965D3C050FB01B` appears at a
2880x1620 render viewport with depth range 0..0. All bytecode is locally
available under Epic `edvr_logs/shaders`; these are classification failures,
not evidence of missing projection-source buffers or a failed DLSS evaluate.

Actual bytecode classifies all five using existing layouts. VS 71DD, 3530 and
19F7 form clip position by scalar-weighted CB1[270..273] columns; their CB0
local transforms remain untouched. VS 9FFA uses four dot products from
CB0[4..7]. VS A52E has the same CB2[11..14] inverse clip-ray calculation as the
qualified F8FA sky variant and the same PS companion; only its exposure output
differs. Surface/cloud pixel companions use pixel-grid noise and lighting
transforms, with no additional projection matrix to patch. Add exact pair
mappings with the existing ownership checks and layout math. No broad
unknown-shader admission or resize workaround is justified.

Restart confirmation: `edvr_gfx_20260925_102718.log` is also verified against
`22fe85d2`. The menu continues history normally (437/437 frames, zero resets at
10:27:59). The cockpit repeats four of the same unknown pairs (19F7, 71DD, 9FFA
and 3530); A52E is absent in this run. Every cockpit reporting window has zero
jitter and resets each treated frame, with backend-failure=0. Ruled out: stale
resources retained across ResizeBuffers, because a fresh process reproduces the
same unknown-pair coverage failures. F10 `20260925_162909_333_27076_1` saves
two complete final samples without failures. Render dimensions are 2880x1620
into 3840x2160 output, so this restarted run uses 0.75x DLSS rather than
native-scale DLAA. The user reports changing only display mode and resolution;
do not infer an additional manual SS change.

The focused collector policy rig compiles and passes all existing math and
viewport checks plus the five exact-pair mappings and rejected companion
identities. The new inverse-sky pair also passes the existing owned-HDR 0..0
viewport contract. Full build and flight validation remain separate gates.

## 55. Station, hangar and concourse coverage audit (2026-09-25)

Verified Epic `edvr_gfx_20260925_103859.log` against installed `ad7607c6`,
build `6AB6A2CD`. The prior five mappings passed full validation and clean DLL
promotion before installation. Sean reports good ship/station visuals and
requests a coverage audit of docking, hangar and concourse F10s. This audit
does not change the installed rendering code.

There are 31 distinct unrecognized VS/PS pairs (29 distinct VS hashes), with no
unknown compute projection observed. One depth-only draw has no PS; this is
distinct from a bytecode capture failure. Unknown pairs first appear at
10:41:02, then more station/interior pairs at 10:43:06 onward. Coverage is not
continuously broken: a clean jittered interval lasts from approximately
10:42:05 to 10:43:06, reaching a treated streak of 5,551 frames. Once the later
pairs appear, repeated unknown-recipe failures force zero jitter and per-frame
history resets. Backend-failure remains zero throughout.

| F10 arm (local time) | Saved final images | Draw evidence |
| --- | --- | --- |
| 10:41:23, frame 63034 | Frame 63035 complete; 4K byte cap stops at one sample | 436/434 draws retained; motion auxiliary evidence partial |
| 10:43:21, frame 73528 | Frame 73529 complete; 4K byte cap stops at one sample | 512 of 1525/1524 draws retained; motion auxiliary evidence absent |
| 10:44:47, frame 81054 | None; arm expires after 900 unqualified frames | No qualified frame |
| 10:45:16, frame 82553 | None before log ends; no completion or expiry recorded | No qualified frame through the last recorded retry |

The first two samples have complete final motion/depth resources, but their
per-draw capture limits prevent claiming complete draw-level coverage. The
later F10s still collect shader/projection diagnostics even though the image
capture never qualifies. All reported samples are native 3840x2160.

A separate persistent refusal starts at 10:44:03.245, frame 77125:
`hdr-camera-changed`. The reference draw is `68DDDEF04D9894AF/06332CA168B6DA63`
with viewport depth range 0..0; the next conflicting draw is
`88DCF1164C640EC3/494506A63091DF8C` with range 0..1. Both write the same HDR
target and use the same DSV and b1 buffer identity, but b1 is rewritten between
draws. Rows 270..272 change XY scale, and 273.z changes from 0.025 to 0.0675.
The treated count freezes at 17,821 through the end of the log as
`conflicting-hdr-target-or-camera` refuses the handoff. Adding projection
recipes alone cannot resolve this separate issue.

Ruled out: the later missing F10 images proving capture hotkey failure; the
arms and automatic shader saves are recorded, while every candidate frame is
refused by the HDR camera contract. Do not bypass this contract merely because
the render target and depth pointers match.

Bytecode classification: 30 projected pairs use existing layouts; one screen
composite is unchanged. No additional PS projection matrix was identified.
These classifications do not establish object-motion coverage or authorize
mixed-camera draws. Exact inventory (instruction numbers are D3DDisassemble):

| VS | PS | Recipe | VS instruction proof |
|---|---|---|---|
| EB5234DB6ADB491D | DC603C35BBE74B31 | ForwardColumns VS b1 row270 (byte4320) | 136..141 |
| 637C27B86091BD60 | 48D45E37C62839E9 | ForwardColumns VS b1 row270 (byte4320) | 120..125 |
| C171BD0C4B585221 | 6855D1919FC5E0C0 | ForwardColumns VS b1 row270 (byte4320) | 120..125 |
| 436193B352A2897E | 51EE1F922FD220B0 | ForwardColumns VS b1 row270 (byte4320) | 136..141 |
| 4D24A7A6C2D12733 | B70DF49F678E806F | ForwardColumns VS b1 row270 (byte4320) | 209..214 |
| DE545DC8EE4FBB87 | A6070F9DD1CFB601 | ForwardColumns VS b1 row270 (byte4320) | 119..124 |
| 98397963AAEC45D3 | 8717694A527EC745 | ForwardColumns VS b1 row270 (byte4320) | 119..124 |
| D005EBB14A22EA0E | 302226F2D8C0938A | ForwardDp4 VS b0 row4 (byte64) | 81..86 |
| D005EBB14A22EA0E | E92C14AA3E51C743 | ForwardDp4 VS b0 row4 (byte64) | 81..86 |
| E308565BF97FDE0B | 0544F1CC95FD1F12 | ForwardColumns VS b1 row270 (byte4320) | 208..213 |
| 5B0068AF5630F96B | A5E2331517988BD8 | ForwardColumns VS b1 row270 (byte4320) | 104..109 |
| 24DE25E496342EB8 | 1A53D2791C12CE92 | ForwardDp4 VS b2 row10 (byte160) | 63..69 |
| 1C5062229AA40CE4 | 2519C9050946D545 | ForwardColumns VS b1 row270 (byte4320) | 19..27 |
| ABF539A8C5CCC1B7 | 3F71C89CA34DF25B | ForwardColumns VS b1 row270 (byte4320) | 96..101 |
| 2B3F53DDA00256E2 | B2DE0A41A4C2B4F5 | ForwardColumns VS b1 row270 (byte4320) | 138..143 |
| 38470D38E07CBDEB | 0A80DFD89B15A05B | ForwardDp4 VS b0 row4 (byte64) | 77..82 |
| 6D8886012A4C6785 | 6F3252AB8579C1E3 | ForwardDp4 VS b0 row4 (byte64) | 75..81 |
| B018D143700AB803 | B403F48CB35D9739 | ForwardDp4 VS b0 row4 (byte64) | 75..78 |
| 899165B9EE284E74 | 26EA0826BD6824E6 | ForwardColumns VS b1 row270 (byte4320) | 167..172 |
| B121A79E457669E8 | 777BF099CBAA7C50 | ForwardDp4 VS b2 row10 (byte160) | 70..76 |
| A7339D1F8A5AC0D0 | D3891373E13BAD40 | ForwardDp4 VS b2 row10 (byte160) | 62..65 |
| C2208C162D010083 | 0000000000000000 | ForwardColumns VS b1 row270 (byte4320) | 85..90 |
| 44C290CC444D1EBE | 3154942271AD5810 | ForwardColumns VS b1 row270 (byte4320) | 4..12 |
| 44C290CC444D1EBE | FD32C5433BD4C015 | ForwardColumns VS b1 row270 (byte4320) | 4..12 |
| EB686C4180DFC6A6 | B11CD77D729C2AEE | ForwardColumns VS b1 row270 (byte4320) | 4..12 |
| 0B71713BCDE4B6C0 | FA7411BF7E4C4088 | ForwardColumns VS b1 row270 (byte4320) | 36..41 |
| B553BB479B7C0B97 | 68ABCB9FEF6CA66C | Unchanged | 11..21 |
| AFFEF0187F1EBC9F | 41152F82C6E8BE1F | InverseScreenRay VS b2 row41 (byte656) | 3..10 |
| A1B7CFCD0BE7493E | 992DE24C01E04A27 | ForwardDp4 VS b2 row10 (byte160) | 62..65 |
| 889A5279E68F0672 | F70549D991FF0E9B | ForwardColumns VS b1 row270 (byte4320) | 135..140 |
| 76ED1E4F8C72C26E | 7ECF7C83FD5AD373 | ForwardDp4 VS b0 row4 (byte64) | 78..83 |

B018/B403 is projected geometry even though B403 also accompanies an inert VS.
AFFEF reconstructs a ray from CB2[41..44], so its fullscreen shape is not proof
that it is unchanged. The four b2 forward cases pass view-position varyings
used by PS depth reconstruction; keep the existing forward math. The B553
screen composite has no camera/depth reconstruction and must not receive a
geometry projection patch.

The on-foot 88DCF shader does not consume changed b1[270..274]; its only b1
read is [275].xyz, and its actual projection is b0[4..7]. However, the existing
reference comparison reports 900 unmatched samples in the hangar and 247 on the
concourse, with spatial/depth differences up to 0.3931. Ignoring the b1
conflict without understanding b0 would risk using scene motion for a genuinely
different projection. Existing raw matrix probes target other shader pairs, and
no qualified on-foot pixel capture is available.

Next diagnostic: during F10, before frame refusal, capture the conflicting
draw's b0[4..7], b1[270..275], named scene camera, reference HDR camera, actual
CB binding ranges/write provenance and depth/stencil state. Compare b0 with
both camera bases to distinguish an incidental bound camera from a real
alternate projection. Keep rendering safeguards unchanged until this is
established. The current audit changes documentation only; shader blobs,
disassemblies and game captures remain local.

## 56. Station/concourse recipes and on-foot camera probe (2026-09-25)

Main `411751ec` merged cleanly (hologram reticle depth, GPU census timing and
the rig debug-layer fixes; no conflicts). The section-55 inventory is now
classified in `flat_projection_recipes.h`: 19 pairs patch VS b1[270..273]
ForwardColumns, six patch VS b0[4..7] ForwardDp4, four patch VS b2[10..13]
ForwardDp4, and AFFEF/4115 patches VS b2[41..44] InverseScreenRay. B553/68AB
is bytecode-unchanged screen composition. The depth-only C220 pair keys on a
null PS. B018/B403 keeps its b0 projection recipe while 5321/B403 stays inert:
the shared constant-output PS does not classify the skinned VS. Every pair
carries exact-companion and cross-identity rejection tests.

The InverseScreenRay math gains a regression with a nonzero third row: AFFEF's
captured instructions add both CB2[43].xyz and CB2[44].xyz before the
view-orientation transform, and compensation still touches only the fourth
row. The displaced-sample identity holds for the two-offset ray.

The on-foot conflict diagnostic from section 55 is implemented as
`flat_camera_probe.h` plus `captureCameraConflict`. During the F10 audit only,
an exact 88DCF1164C640EC3/494506A63091DF8C draw whose frozen camera differs
from the selected target's tone reference (the `hdr-camera-changed` condition)
is sampled before observation and refusal. The record verifies actual VS/PS
binding, prints the reference-HDR, current-draw and named-scene frozen camera
rows side by side, reads the current VS b0[4..7] and b1[270..275] shadow
slices through the D3D11.1 range getters with full provenance metadata
(tracked/width/generation/mutation/mapped/pending/shadow write and epoch), and
dumps the depth-stencil state. A post-observation line names the target's
first-bad witness and whether this draw caused it. The new
`constantsMetadata` accessor is read-only: it tracks nothing, allocates
nothing and queues no readback.

The budget is two attempts in distinct increasing frames per F10 arm. The
summary's result word distinguishes exact-pair-never-observed,
observed-without-HDR-camera-conflict, actual-shader-mismatch,
partial-missing-evidence, captured and conflict-without-capture, so an
inactive probe cannot read as success. CPU shadows only: no GPU readback, no
plan binding, no camera admission and no rendering change. The
`hdr-camera-changed` refusal stays in force until this evidence lands.

Full build `build/flat-station-camera-probe-build.log` passes: 82 pooled jobs
plus four quiet jobs, the 260-key config contract and both installer payload
checks, including the 30-pair station census, exact-companion rejection and
the AFFEF regression in the flat rigs. Next Epic flight: dock, walk the
station interior and go on foot with F10; the formerly unknown pairs should
prepare instead of resetting history, and the probe should report captured
evidence for the 88DCF conflict frames.

## 57. On-foot weapon camera qualified; residual pairs mapped (2026-09-25)

Verified Epic `edvr_gfx_20260925_122208.log` against installed `d0898e1b`,
build `6AB6B35F`. Sean reported continued aliasing/shimmer on foot in the
hangar and concourse after section 56. The log confirms the mechanism: treated
2,926 versus 14,644 refused, and on-foot windows refuse 247-337 copies per
five seconds with `conflicting-hdr-target-or-camera` /
`hdr-camera-changed`; every refused copy invalidates history, so on-foot
frames never accumulate. The earlier station/ship intervals kept their long
treated streaks (1,996) and backend failures stayed zero.

The camera probe captured both attempts in each of two F10 arms
(`result=captured`, complete=2, missing=0). The conflicting draw is exactly
`88DCF1164C640EC3/494506A63091DF8C`, about one draw per frame, writing the
selected HDR target and named depth at the full 3840x2160 viewport with depth
range 0..1, depth-write on, GREATER_EQUAL. `eye_draw_snapshot.h` has listed
that VS with the laser-rifle family since 2026-09-17: this is the
first-person weapon pass.

Its projection is a real second camera, not stale bytes: same camera position
(row 275 identical) and same clip-W column (row 274 identical) as the named
scene camera, with XY scale about 1.23x tighter and near 0.0675 versus 0.025.
Its VS b0[4..7] is the transpose of its own b1[270..273] current values --
self-consistent in both layouts. The runtime's refusal was correct under its
contract; the frame-level cost is what made the whole on-foot image shimmer.

Change: in `flat_runtime_model.h` the exact weapon pair neither sets nor
vetoes the scene camera. Everything else about HDR ownership stays strict,
and the weapon keeps its existing b0[4..7] ForwardDp4 recipe so it renders at
the frame's phase under its own projection. At resolve, its pixels fail
strict depth ownership against the scene camera and fall back to current
colour: no history smear on the weapon, at the price of possible local crawl
on the weapon itself under fast movement. That trade is the qualification
target for the next flight; a weapon reactive mask or second-domain history
remains available if it shows.

The same flight captured bytecode for the four remaining unknown pairs (six
stages saved, zero failures). Offline classification
(`build/flat-audit-station/`, ignored): `0A298DE7DF833A46/6FD4C38BA927C8C7`
and `D8FCE3CEA16B9B51/06AA136E4D58CBA2` are decal passes projecting through
cb1[270..273] columns; their PS derives the depth-texture UV from the
exported view-position varying, so VS and depth-pass projections must stay
aligned. `BA16062A2EB66F1F/33758387B70944A1` writes the HDR target and is
skinned (t38 bone loop); projection is the last step on the world position,
cb0[4..7] dp4. `66DE2CADB1F4AE6B/BBDE4E71FB78528A` is the multi-UV hull VS
(dual rigid/skinned path) with a new PS companion, cb1[270..273]. All four
have exact recipes and census tests. These recipes do not establish motion
coverage; the skinned HDR draw's motion source is still the open question
shared with the VR concourse NPC observation below.

Regression coverage: the flat rig replays a changed-camera HDR draw that must
still refuse as `hdr-camera-changed`, and the same draw as the weapon pair
must now select. The recipe census covers exact companions, cross-identity
rejection and the absent-PS case.

Separate observation, recorded for routing: Sean saw aliasing on concourse
NPCs in VR on `d9f86b09` (`claude/openxr-perf-gaps`), not on this branch's
build. NPCs are skinned world meshes; VR motion covers rigid engine records
and the view weapon's skinned vertices, while world skinned content falls
under the reactive policy. Whether that observation is a longstanding
reactive-coverage gap or a regression belongs to the main/openxr-perf-gaps
line; this branch's own VR regression gate remains open regardless.

Merged to main at `dacb7a56` on 2026-09-25 with main `a4cdb045` included
(clean ort merge; hologram-depth resolve and native-device retention changes).
The merged tree's first full build flaked once in `scheduler_stack_probe_test`
("the exemplar kept the real depth"), a rig the merge does not touch; the rig
passed standalone three times and in the full retry, so it is recorded here as
a load-sensitive flake alongside the run_jobs and vtable history rather than
chased further.

## 58. Reset-storm symptom is scene content, not the live mode change (2026-09-25)

Sean reported that changing `fix.temporal_aa` from the F8 menu made flat
temporal AA disengage until restart. Verified Epic `edvr_gfx_20260925_193443.log`
with `tools/edvr_log.py --expect-build HEAD`: `v0.18.0-rc.2`, build `6AB6DC53`.
Menu writes ran 19:35:00-19:35:55 from the main menu (plus further toggles to
19:36:38); the scene came up at 19:35:15 (frame 32172). From that frame to
session end, every treated frame was a full reset: `accepted-history-5s=0`,
`longest-treated-streak=1`, per-frame `requested=1` in the resolve reset
events, and the phase census reports 361/438 frames failed in the first storm
window (reasons: `unknown-scene-projection-recipe`,
`projection-preparation-refused`). The adapter's own reset counters
(no-previous/frame-gap/depth/color/extent) stayed near zero, so the resets
came from the jitter-validity term at `flat_runtime.cpp:1423`
(`s.jitterWanted && (s.phase.failed || !s.phase.previousAcceptedValid)`):
each unreciped scene draw calls `failPhase`, so no frame ever finishes clean,
so history is never offered to the next frame. A temporal backend reset every
frame displays current-frame-only output -- visually identical to AA off.

Ruled out: the live mode/model change as the cause. (a) The 14:18 session on
`84733e9c` changed off->on->dlss live on foot and was `treated-jittered` with
`accepted-history-5s`~450 within five seconds. (b) The 10:38 session on
`ad7607c6` stormed before, during and after its live changes
(dlss->fsr->off->on->dlss, model k->j->l->m) and recovered mid-session at
10:42:05 when the unreciped content left view, with no restart; the same storm
recurred at 10:43:29 when it returned. (c) `84733e9c..ac2e0b29` changes only
menu.cpp's FPS-readout clock and release packaging -- no temporal-path code.
(d) The 19:34 session was itself a fresh launch and stormed from the first
scene frame, so "restart fixes it" is not supported: restart only changes what
is on screen. Every menu write was applied at the next frame boundary
(`mode=`/`DLSS model=` lines track each change); the live-apply path is
healthy.

The storm is content-driven. New unknown pairs, automatically captured with
exact creation bytecode saved under `edvr_logs\shaders` (six stages, zero
failures, verified present on disk):

| Pair (VS/PS) | Target |
| --- | --- |
| `AACFDCF2FB9AD809` / `CAD1F585EDDC5641` | fmt 23, 3840x2160 |
| `0357BBB2DEE43C1F` / `BE02244365AD810C` | fmt 26, 3840x2160 |
| `361CD4B7FF213A01` / `CDDFE2157F5654B8` | fmt 23, 3840x2160 |
| `525D47E3D5E2EFF4` / `0D617929FED842F0` (17:36 session) | fmt 26 |
| `EB5234DB6ADB491D` / `63B1524A9F805A4C` (17:36 session) | fmt 23 |

These are not the section-57 four (those have recipes since `dacb7a56`).
`361CD4B7FF213A01` already has a recipe with PS `FA7411BF7E4C4088`; the game
pairs the same VS with a new PS here. `AACFDCF2FB9AD809` is the known
engine-motion family VS and `0357BBB2DEE43C1F` the documented b2[10..13]
projection family, so recipe development can follow the section-24/57 process
offline without another capture flight.

Adjacent gaps noticed, recorded for routing, not folded in: the disengage is
silent in game -- the only signal is the log's jitter-refusal and reset-event
lines -- and a single permanently on-screen unreciped pair poisons history for
the whole session regardless of how few pixels it touches.

## 59. Main-menu reset storm is EDHM's patched pixel shaders (2026-09-25)

Sean's toggle flight pins the section-58 storm's variable: EDHM. Both broken
rc.2 sessions (17:36, 19:34) chain `d3d11_edhm.dll` (41 exports); the 21:08
no-EDHM control session at the same main menu has zero unknown-pair captures
and accumulates normally (`treated-jittered`, accepted-history-5s ~430,
growing streaks). EDHM patches pixel shaders, which changes their exact
creation hashes: all five section-58 pairs are known VS hashes paired with
unseen PS hashes. Pairs 1-3 declare EDHM's `t120` 1D lookup table (absent
from every stock companion); the 361C companion diff against stock
`ps_FA7411BF7E4C4088` shows the mod's ~29-line colour block and register
renumbering beside the original code.

Classification of the captured bytecode (listings in ignored
`build/flat-audit-menu/`): none of the five new PS blobs consumes a
projection. CAD1 (pair 1) and 63B1 (pair 5) read only the CB1[277..279]
orientation rows; CDDF (pair 3) uses texcoord UVs and scalar config reads;
BE02 (pair 2) reads vPos only as an integer pixel-grid lookup and rebuilds
position from a view-space varying formed from CB2[2..4], not the patched
CB2[10..13]. Pair 4's VS remains inert (no constant buffer at all) and its
new PS filters screen colour with a vPos-scaled mask, so that pair joins the
explicit unchanged list beside its stock companion F0BA.

Change: exact recipes for the four projected pairs -- AACF/CAD1, EB52/63B1
and 361C/CDDF as CB1[270..273] ForwardColumns, 0357/BE02 as CB2[10..13]
ForwardDp4 -- plus the inert 525D/0D61 classification, with census coverage
in the flat rig (exact-pair admission, wrong-PS/wrong-VS/absent-PS refusal,
unchanged-list exactness). Hashes are stable per EDHM configuration, so this
unblocks Sean's current EDHM setup; an EDHM settings change can mint new PS
hashes and re-trigger the storm, which the log's unknown-pair capture will
continue to report.

Open, recorded for routing and deliberately not folded into this change:
exact-hash recipes cannot keep pace with mod-patched shader populations --
Sean reports the same failure class from ReShade users. The systemic answer
is a runtime PS-safety classification at the unknown-pair path (the
creation-bytecode cache already retains the bytes), which is a design change
needing its own qualification. The disengage also remains silent in game:
the only user-visible signal is AA looking off, and the evidence lives only
in the log's jitter-refusal and reset-event lines.

## 60. Maxed-settings capture: four stock lighting variants (2026-09-26)

Sean ran the all-settings-maxed capture on Epic (`edvr_gfx_20260926_054653.log`,
build `6AB73CBB` = rc.2-5-g58974c5a, EDHM chained). The session first shows
flat AA working with EDHM at his previous settings: `treated-jittered` at
05:48:04 with accepted-history ~257/5s and a 71 streak. After the settings
were maxed, four unknown pairs appeared in one burst at 05:48:41 and the
section-58 storm resumed. None of the four PS blobs declares EDHM's t120
table: these are STOCK variants minted by the settings tiers, not mod
patches -- the first direct capture of the settings-variant mechanism a
user report (20260926_121716 bundle, unreciped glare `3D05E7CF11AC9BEE` and
deferred-UI blend `F512712C40D93C12/4A71EB0D34E9F2EF`, no mod chain)
pointed at.

Bytecode classification (build/flat-audit-menu): all four pairs are clean.
AFED (F512 companion, 2513 instructions) is clustered forward lighting for
glass; 3B0B (EB78 companion, 2081) is gobo/projector-quad forward lighting
whose projective divides are LIGHT-space cookie projections (cb1[165..172]),
not camera rows; 3D84 (24DE companion) and 70E6 (0357 companion) are
deferred light passes using the same integer pixel-grid depth reads and
view-ray/`cb2[2..4]` reconstruction the table already accepts for their
stock companions. vPos appears only as integer tile/pixel-grid lookups. No
DoF/bokeh gather kernels materialized in this batch; blur/DoF passes remain
unqualified and are the expected source of the next settings-tier captures.

Change: exact recipes for the four pairs -- F512/AFED and EB78/3B0B as
CB1[270..273] ForwardColumns, 24DE/3D84 and 0357/70E6 as CB2[10..13]
ForwardDp4 -- with census coverage in the flat rig. The settings-maxed
flight with this build should now hold treated streaks where 05:48:41
stormed. The bundle user's two pairs still want his blobs
(`vs_3D05E7CF11AC9BEE`, `ps_4A71EB0D34E9F2EF`) from his machine, or the
generic classifier, to cover.

## 61. The maxed-settings chain: DoF-composite tone variant (2026-09-26)

The settings-max failure had a second, larger half the recipes could not
touch: with the settings maxed the selector refuses every frame with
`no-known-tone-pass` -- the post chain itself changed. The config diff
(Sean's fxcfg vs his 2026-09-21 baseline) names the maxed set: BlurEnabled
on, DOFEnabled 2, BloomQuality 3, AOQuality 3, shadow/texture-filter tiers
up. Bloom-off did not restore the chain in the menu.

Two instrument builds (`9e27e7f2`, gated by `20b701e6` after the first
burned its budget on startup's empty-table refusals) added a bounded
handoff-chain dump on tone/copy refusals, with creation-bytecode dumps of
the handoff records. One 30-second main-menu visit produced the whole
chain (`edvr_gfx_20260926_073622.log`, frame 33455): a plain swizzle copy
at q=36, the 1920x1080 DoF blur chain at q=66/106, then at q=130 the tone
slot written by the KNOWN tone VS F9CFC798F21E9AEA with a NEW PS
DE65BFFF2F12ECC6, then the known copy (20F3/DED879) at q=133, an LDR
grade/grain composite (20F3/67A1C6A38826030A) at q=140 that runs after
EDVR's handoff and is benign, and the known panel at q=148.

Bytecode review of the three dumped PS blobs (build/flat-audit-menu): the
tone variant blends the HDR (t0) with the quarter-res DoF blur (t1) by a
VS-varying/depth-derived factor, both sampled at unchanged UV; no depth
texture, no SV_Position, no CB matrix -- the same jitter contract class as
the stock tone, which is why bloom-off did nothing (the composite is DoF,
DOFEnabled=2). Its HDR lineage is at PS0, where the stock tone's is at PS1.

Change: the selector admits `kToneDofCompositePs` as an alternate tone PS
for the same tone VS and routes HDR lineage through the variant's actual
slot (PS0), with a rig fixture covering selection with HDR at PS0; the
stock tone's PS1 requirement stays exact. The next menu flight with maxed
settings should select and treat; what remains open after it is the
visual qualification of the post-copy composite and motion blur (the
temporal contract does not reproject via the game's motion blur), plus
the 1920x1080 chain's blobs if their classification is ever needed.

## 62. Generic shader-pair admission by bytecode classification (2026-09-26)

Exact-hash recipes cannot keep pace with mod-patched or settings-tier shader
populations -- the EDHM storm (section 58), the maxed-settings variants
(sections 60-61) and the supporter's stock-pair storm all say so. The flat
runtime now classifies an unreciped scene pair's actual creation bytecode
once per (vs,ps) pair per session (64-entry memo, no allocation on the draw
path) and admits provably-safe pairs through the identical qualifyProjection
flow: a VS the analyzer can prove is a forward column-sum or dp4 clip idiom
with NO second matrix use, branch or inverse consumer, paired with a PS that
has no depth output, no vPos float path beyond integer tile/pixel-grid
lookups, no clip-varying depth-UV idiom, no multi-row cb combine and no
unanalyzable involvement. Inert-no-CB VS + clean PS joins the unchanged
class. Audit outcomes 105/106 name generic admissions; everything unproven
keeps the capture+failPhase path unchanged.

The walker is length-safe (advances by the instruction-length field only);
corpus sweep over 340 captured blobs: zero desyncs, all 58 exact-table
forward recipes cross-matched on slot/layout/row, and every known
inverse/special family (sky, deferred ray, screen ray) and consumer PS
(7EAC, 8DEF, the decal companions) refuses. Known under-coverage, all
conservative: light-space-matrix lighting PSs (the gobo/deferred variants
from section 60) and clip-varying depth-UV decal pairs refuse generically --
the exact table covers their stock cases, and their EDHM rehashes would
refuse and capture. The two-conditional-matrix and inverse families stay
exact-hash territory.

This is also the supporter-bundle answer for stock-pair storms (his glare
and world-blend pairs need no blobs from him once this ships). Separately,
the supporter's second bundle shows a different failure class entirely:
after switching to the VR edition his sessions die at
`module_startup,graphics_unavailable=80004002` with no `runtime,` line --
the OpenXR runtime never answered (no active headset/runtime), the same
five-line signature as the 2026-09-14 doc's 21:16 not-a-flight case, and
the process was killed before hook confirm, so the next session ran
sentinel-disabled and looked worse. His flat problem was the section-58
storm; his VR edition choice was the wrong tree entirely.

## 63. The tone chain joins the variant treadmill (2026-09-26)

Sean's main-menu Epic flight with EDHM chained (`edvr_gfx_20260926_124418.log`,
build `bf7a3e18` -- the section-62 tree) cycled every F8 AA mode with no
visible change. The scene side is healthy: zero unknown-pair captures all
session, the generic classifier preparing EDHM's patched scene pairs
(AACF/CAD1 et al). The failure is the handoff chain: every frame refuses
`no-known-tone-pass` (treated=0, streaks 0; `request=` follows the F8
cycling, so the request path is live). The 07:57 session on `0a3dc038` --
the build with the section-61 DoF-composite admission -- shows the same
chain and also never treated, so this tier predates the flight that
reported it.

The chain dumps name the cause. Frame 33504: the tone target written at
q=156 by VS `CFA91824129ECBBC` / PS `9270C355389DA302`. Frame 33939: the
SAME target written at q=948 by the known tone VS `F9CFC798F21E9AEA` with
PS `EAA5F18F10533BD1`. The selector knew one VS and two PS hashes; the tone
slot in fact alternates writers across frames, and the known output copy
(20F3/DED879) sits after both. Bytecode reviews (build/flat-audit-menu):
9270 is a tonemap with the bloom composite folded in -- t0 HDR and t1 bloom,
both sampled at unchanged UV, no depth texture, SV_Position or matrix -- HDR
at PS0, the DoF-composite's class. EAA5 is EDHM's recolor grade folded into
the tone -- t1 HDR and t2 bloom at unchanged UV, a grade LUT (t0) applied
after tonemapping, t120 the mod's config table -- HDR at PS1, the stock's
class. All three tone VS are camera-free fullscreen passthroughs (CFA9
no-const, F9CF sampling t0 for its varying z, 43CA reading cb2[2].y),
interchangeable in the role; the frames mix them against the PS variants.
`ps_EBE0E51C47113BE7` is EDHM's recolor running as a post-copy output draw
-- the section-61 benign class, no admission needed.

Change: tone admission decouples the axes -- a tone VS set (kToneVs,
kToneVsNoConst, kToneVsCbZ) against a per-PS HDR-slot table (kTonePs and
kToneEdhmGradePs at PS1; kToneDofCompositePs and kToneBloomCompositePs at
PS0) via `toneHdrSlot()`. The draw scope's SRV capture and the runtime
model's tone counting use the same helper; both were exact-stock-pair, so an
admitted variant would have selected with empty SRV bindings and counted no
tone. Latent fix: `FlatMonoFrame::hdr` hardcoded `srvResource[1]` -- wrong
for every PS0 variant, never exercised because none had selected live -- now
routes the variant's slot, and the DoF rig fixture gained distinct per-slot
tokens to pin it. Rig coverage: the four observed accept shapes (DoF, bloom
tier, EDHM grade, mixed VS) plus unknown-VS/unknown-PS refusals.

Open: the tone chain is now on the treadmill section 62 retired for scene
pairs -- four tone PS variants in one day, and an EDHM or settings change
mints the next one. The systemic step is a generic tone-slot admission:
structural position (a single fullscreen format-27 draw writing the copy's
exact source, immediately before the known copy) plus the bytecode safety
class (unchanged-UV sampling, no depth texture/SV_Position/matrix), the HDR
slot proved by which bound resource carries conforming format-26 records
rather than by hash. That wants its own qualification; unreviewed variants
keep refusing and dumping. Next flight: this install's Epic main menu with
EDHM at current settings -- expect `treated-jittered` with growing streaks
and no `no-known-tone-pass`; the section-57 on-foot flight stands after it.

## 64. Tone admission flown; upscalers were a build-environment gap (2026-09-26)

The section-63 tree's first flight (`edvr_gfx_20260926_131921.log`, build
`rc.2-13-g2bd1a8d0-dirty` -- the pre-commit full build; the promotion had
failed on the supporter-logs fingerprint, and this section's install
supersedes it with the clean `rc.2-14-g8103620f`): DoF off, EDHM chained,
the main menu. The tone admission works: exactly one `no-known-tone-pass`
all session (a startup frame), and every `mode=on` window from 13:21:07
treated -- `treated-jittered` with live phases, accepted-history climbing,
longest streak 446.

FSR and DLSS did not engage, and the cause is not the code: this dev build
carried neither upscaler SDK (`temporal=this build has no DLSS SDK in it`,
spatial fallback; the FSR3 banner refused at build time). The machine now
carries both pinned SDKs (`%LOCALAPPDATA%\EDVR\ngx-sdk` 310.9.1,
`ffx-dx11` 3.1.2; the FSR port's cmake build needed a portable cmake, now at
`%LOCALAPPDATA%\EDVR\cmake`). A full-pass rebuild at `8103620f` with both
SDKs is installed on Epic with the pinned runtime; the next flight should
see DLSS and FSR engage wherever TAA did.

One open watch item the flight exposed: a ~50 s
`conflicting-hdr-target-or-camera` storm from launch (the runtime conflict
lines name `missing-depth-or-dsv` writes into the tone's HDR resource by the
1920x1080 chain's `129F602B2A9CA439/8826CACC6382C78D` and a copy-variant
`20F383BBAC05C031/BF2302BCC7B434DB`). It cleared by itself before the first
treated window; the refusal is the HDR identity guard working while
provenance is unprovable, at the price of AA standing down in those states.
If it recurs in steady menu or flight states, qualify the alias (extent
mismatch, ordering) instead of refusing; do not loosen the guard on
unmeasured evidence.

## 65. In-flight reset storm: 16 pairs reciped; transition HDR alias open (2026-09-26)

The 13:46 Epic flight on the SDK-full build (`rc.2-14-g8103620f`) treated
the entire main menu, DLSS included (`treated` past 16k, accepted-history
~400/5s) -- sections 63-64 closed. In flight states the session
reset-stormed: 58 `unknown-scene-projection-recipe` refusals, streaks at
zero. The automatic audit captured 16 distinct pairs with all 32 bytecode
stages saved, no F10 needed.

Fifteen are known VS families with unseen PS hashes -- the section-59
pattern at flight scale. Eight are EDHM-patched companions (t120 present;
the delta against the vetted stock companion is the mod's colour block and
config branches, instruction-for-instruction otherwise, cb row profiles
identical). Four are stock settings-tier lighting variants (7AA0, 057F,
1AE6, 9887) whose profiles match their companions exactly; their single
svPos use is the companion's own integer tile divide. Three small ones read
cb2 colour config only. `62FB9466` is EDHM's HUD recolor tree (~990
instructions of literal t120 region tests) over the 14-instruction stock
glare PS, sampling t0 at unchanged UV. The sixteenth pair is the
EDHM-patched E904 glare VS (`7F894EB5`): the mod's t120 block scales glare
size/alpha upstream of position; SV_Position remains the cb0[4..7] dp4
idiom, with cb0[9..11] a view-space billboard orientation -- a local
transform, not a clip consumer.

Change: exact recipes for all 16, mirroring each VS family's measured span
(the flight census in the recipe rig: admission, wrong-PS/wrong-VS/
absent-PS refusal, no duplicates). The generic classifier's verdicts were
the documented conservative refusals -- EDHM t120 loads, billboard second
matrices, and lighting vPos divides it cannot prove; the exact table
carries them as designed.

Open: the `conflicting-hdr-target-or-camera` storm is now characterised as
TRANSITION-phase resource aliasing, launch and menu-to-flight instance
change, always the same two depthless writers into the tone's HDR resource:
the 1920x1080 chain's `129F602B2A9CA439/8826CACC6382C78D` and copy-VS
`20F383BBAC05C031` with an uncaptured PS `BF2302BCC7B434DB`. It clears in
steady states and selection proceeds; BF23's blob is the missing evidence
if it ever persists. Next flight: fly with EDHM at current settings --
expect zero unknown-pair captures and treated streaks once the transition
settles; if the alias outlives transitions, qualify it by extent mismatch
instead of refusing.

## 66. Tone HDR slot in the online model; the conflicting-hdr storm re-attributed (2026-09-26)

Commit 8103620 taught `flatSelectMonoFrame` a per-variant tone HDR slot via
`flat_mono_detail::toneHdrSlot(vs, ps)`: PS1 for the stock tone and the EDHM
grade, PS0 for the DoF composite (`kToneDofCompositePs`, DE65) and the bloom
composite (`kToneBloomCompositePs`, 9270). The online runtime model never got
it: `flatRuntimeObserve`'s copy branch (flat_runtime_model.h, two
`srvResource[1]` reads) and three sites in flat_runtime.cpp (a UAV-write guard,
two log lines) still hardcoded slot 1, so a PS0 variant's own PS1 blur/bloom
read as its HDR and every frame refused.

Fix (`989f6fd`): `flat_mono_detail::toneHdrInput(key)` (flat_mono_frame.h)
returns the recorded (vs, ps)'s actual slot, or null; all five sites now go
through it.

Evidence: a Linux replay of the rig's `MonoFixture` through
`flatRuntimeObserve` on HEAD `3d12d97`. Unfixed: stock and EDHM grade (PS1)
select; DoF composite (DE65) and bloom composite (9270) refuse
`no-observed-hdr-writes`, and `conflicting-hdr-target-or-camera` once their own
blur/bloom chain (`129F602B2A9CA439`/`8826CACC6382C78D`) writes their PS1
texture. Fixed: all six cases, with and without the chain, at both render
extents, select with `hdr` at the true HDR token.

Section 65 characterised the same storm as transition-phase aliasing: two
depthless writers, the 1920x1080 chain's 129F/8826 and 20F3/BF23, into "the
tone's HDR resource". That resource is what the unfixed model read at PS1. For
a PS0 tone it is the variant's blur/bloom texture, whose writers are exactly
such depthless passes; for a PS1 tone (stock or EAA5, the steady menu) it is
the true HDR, which is why the storm clears in steady states. The next flight
on this fix discriminates: gone at launch and menu-to-flight means it was this
bug; still there means a genuine alias of the true HDR, and section 65's
qualification applies.

- ruled out: qualifying the section 64-65 storm as an HDR alias before a flight
  on this fix, because the unfixed model read the PS0 variant's PS1 blur/bloom
  (the texture 129F/8826 and 20F3/BF23 write) as the HDR and the replay
  reproduces that exact refusal; only a storm that survives the fix is a real
  alias.

The rig missed it because its slot-0 fixtures (section 61) only ran through
`flatSelectMonoFrame`, which 8103620 already fixed; nothing exercised those
variants through the separate online model.

With section 65's census, Sean's report splits cleanly: the 13:46 flight's
in-flight "no AA change" was the 16-pair reset storm, and this bug refuses
every frame whose tone is DE65 or 9270 (DoF on, and the launch and
menu-to-flight transitions).

Status: built and tested only in a Linux model replay, NOT through `build.bat`;
the Windows full build must pass before merge. Next flight: DoF on, main menu
and in flight; expect no `conflicting-hdr-target-or-camera` /
`no-observed-hdr-writes` while DE65/9270 write the tone slot, including at
launch and menu-to-flight.

### Generic classifier review (same session)

Verified findings on the generic shader-pair classifier
(flat_shader_classifier.h), recorded so later sessions don't lose them:

- any opcode missing from `operandCount` (resinfo/GetDimensions, case, lod,
  gather4_c/po, sample_info, bufinfo) refuses the whole shader
  (flat_shader_classifier.h:1105 PS, :848 VS);
- the >=3-rows-of-one-cb and cb x cb product rules ignore coefficients (tint x
  colour and three-colour sums refuse), contrary to the header's own rule;
- "anywhere" checks inspect final register states only (the same computation
  flips Clean/Consumer on register reuse);
- min/max drop their first operand (share movc's indices 2/3);
- the SV_Depth refusal is dead (oDepth is declared by dcl_output, opcode 101;
  the walker checks 102/103);
- the div rule only matches a literal div of two varyings; the rig's decal A/B
  refusals come from an indexable temp (6FD4) and the 3-row rule (06AA), not
  the div rule;
- the 64-pair memo and the 2048-shader/16 MiB bytecode cache fail silently;
  verdicts are logged only under F10;
- measured coverage: section 65's flight census, 0 of 16 in-flight unknown
  pairs admitted; all 16 needed exact recipes.

Evidence: Linux harness over the 34 fixtures plus hand-assembled ps_5_0
programs.

## 67. Partial AA: per-draw refusal with a reject stamp; coverage census and classifier reasons (2026-09-26)

Why: sections 54, 59-60 and 65 each lost AA for the whole frame to a new shader
population -- a display-resolution change, EDHM, settings tiers. One per-frame
phase flag promotes every per-draw cause (23 distinct reasons) to a frame
refusal, and coverage is keyed on 13 exact-hash tables. Sean approved the plan
(2026-09-26): (1) coverage census, (2) local refusal, (3) VS-keyed jitter and
motion admission with a PS denylist, (4) tone/copy identification by frame
structure, (5) offline qualification from shader dumps. This change implements
(1) and (2).

Local refusal (`experimental.temporal_aa_partial`, default on, live; off is the
previous behaviour): six per-draw reasons no longer fail the frame's phase --
`unknown-scene-projection-recipe`, `unchanged-shader-mismatch`,
`actual-shader-mismatch`, `projection-viewport-mismatch`,
`projection-preparation-refused` and `draw-binding-refused` (draw path only;
the dispatch path and every other reason stay frame-global). The draw goes out
unjittered; `~FlatRuntimeDrawScope` re-issues it once through the `vScreen*Raw`
bypasses with a no-input stamp PS into an R8 render-size reject mask, under a
variant of the game's depth-stencil state (depth writes and stencil off; EQUAL
when the game's state wrote depth, else the game's own test). The resolve's
`prep()` rejects mask pixels, and `taa()`/`finish()` show them as current
colour sampled at the unjittered uv; the rejection texture already feeds DLSS's
bias mask and FSR's reactive mask. Indirect and auto draws, a missing DSV, a
DSV/mask size mismatch or missing resources fall back to `failPhase`.

Census and reasons: `flat coverage 5s:` is printed every window even when zero
(scene draws by branch, local refusals, stamps, global fallbacks, frames with
stamps, treated frames with stamps, `memo-full`), with up to five `flat
coverage stamped 5s:` pairs and a `flat coverage startup:` line per device. The
generic classifier now names every refusal rule, and `flat generic
classification:` logs each pair's verdict once (at most 64 per session) with
unknown opcode numbers and vPos consumer sub-rules; a full memo is logged once.

- ruled out: a stencil tag for refused pixels, because the game reads the scene
  depth's stencil plane from compute and lighting passes (device_hook.cpp's
  CS-hook note).
- ruled out: jittering the camera buffer at its source, because the same VS b1
  buffer is rewritten with different camera contents within a frame (section
  57's weapon camera) and the same rows carry per-object matrices (section 25);
  per-draw binding costs two bind calls per draw once warm.

Known limits: a refused draw whose PS writes SV_Depth is not stamped by the
EQUAL test; under DLSS/FSR only the 2x2 footprint is forced to current colour,
so backend smear around stamped objects is possible; the re-bind assumes no PS
UAVs, as the producer path already does. Open follow-up: supersampling above
1.0 silently disables DLSS, DLAA and FSR
(`flat-trained-resolve-cannot-downsample`); fall back to TAA and say so.

Verification is Linux-only: a MinGW build of `flat_temporal_test` run under
Wine passes (tone-slot replay, local-refusal policy, classifier reasons), MinGW
syntax checks of flat_runtime.cpp and flat_mono_resolve.cpp pass, and the
config contract passes. NOT verified: the HLSL (Wine's d3dcompiler cannot
compile these shaders; build.bat's fxc step is the first compile) and
vscreen.cpp (MSVC SEH). NOT built with build.bat, NOT flown.

Next flight (Epic, EDHM, current settings, supersampling at most 1.0): main
menu with DoF on and off, flight, station; then change supersampling and
display resolution mid-session. Expect treated streaks to continue through each
change, `stamped` counts where unknown pairs appear instead of
`unknown-scene-projection-recipe` phase failures, and a `flat generic
classification:` line naming each refused pair's rule. Watch for crawl where
stamped and treated surfaces meet, and for DLSS/FSR smear around stamped
objects. A/B with `experimental.temporal_aa_partial = off` (live).

## 68. Local refusal redesigned to one raster phase (2026-09-26)

Implements the partial-refusal repair from
[review-flat-temporal-aa-2026-09-26.md](review-flat-temporal-aa-2026-09-26.md)
(findings 1, 4, 5; its remaining findings route to the staged program
below). Verified before editing: `runtime_profile.h` never permitted
`experimental.temporal_aa_partial` in the flat profile, so the key read
`off` before its default -- the merged stamp-mask partial AA could not have
activated on Epic at all. Findings 4-5 stand as design constraints: mixed
unjittered geometry in shared jittered depth/colour cannot be repaired by
a final mask, and the stamp/replay path carried no ownership guarantees for
discard, SV_Depth, stencil or mask identity.

Change: the flat profile permits the key. A per-draw-local refusal (the six
reasons, unchanged) still lets the draw go out unjittered, invalidates the
frame's history through the existing failPhase coherence -- zero phase
before any application, the spatial fallback after, never a mixed-phase
temporal evaluation -- and returns the runtime to observation: frames run
unjittered and the copy-draw treatment is skipped until a refusal-free
frame requalifies the contract, when the usual warm-up resumes.
`experimental.temporal_aa_partial=off` keeps the previous behaviour
(failPhase retried every frame). Removed: the stamp pixel shader, reject
mask, DSS-variant cache, re-issue path, resolve t9 plumbing and the three
raw-draw bypasses (133 insertions, 432 deletions). Kept: the coverage
census and named classifier reasons, now reporting `observing=`, entries
into observation, frames spent observing and the top locally refused pairs.
The reset storm's shape changes with `on`: a persistent unreciped pair now
holds the runtime calmly in observation (one failPhase on entry, no
per-frame retry churn) instead of storming; the exact-recipe pipeline
remains the resolution path, with the census naming candidates.

Deferred to the review's staged program, not started: three-size routing
and backend negotiation, the FrameContract online/replay reducer, cache
retirement (the 32-plan/64-pair cliffs), proved shader-family contracts
with adversarial fixtures (finding 2), and the end-to-end hooked
composition tests (finding 6). Next flight: EDHM at current settings,
menu and flight; expect treated streaks, `observing=` transitions with
locally-refused pairs named instead of a reset storm when coverage is
incomplete, and no conflicting-hdr at launch or menu-to-flight (a storm
that survives section 66's slot fix is a genuine alias; qualify it then).

## 69. Section-68 flight: observation holds, transition refusals are benign (2026-09-26)

The 18:19 Epic flight (`edvr_gfx_20260926_181905.log`, clean
`rc.2-24-gda9e2c8e`, EDHM chained, DLSS requested): menu and flight both
treat continuously -- treated-streak 3349 with accepted-history ~450/5s in
steady flight. Zero unknown-pair captures all session: the section-65
recipes hold. The one observation episode is textbook section-68: frame
44683 refuses `projection-preparation-refused` on the RECIPED
5453D19B/289C3EA6 (a transient during the transition), returns to
observation, and requalifies one frame later -- calm, no storm churn.

The conflicting-hdr storm survives the section-66 slot fix, so per the
flight plan it needed qualification. Every surviving writer is
transition-scoped: the 1920x1080 chain's 129F/8826 and the copy-variant
20F3/BF23 (`missing-depth-or-dsv`, BF23's blob still uncaptured), and the
transition's video pass at 2496x1404 fmt 9: `image-copy-source` names the
KNOWN HDR copy CFA9/`DFCBA0EC70B03C9B` (kHdrCopyPs; its blob: t0 copy at
unchanged UV plus a luma tap) writing the resource the FCFA image filter
(kImageFilterPs) first wrote. Not an alias of the true HDR -- transition
content at a different extent, where refusing accumulation is correct.
Qualification recorded; no alias relaxation needed. The
2496x1404/video-extent provenance can join the family contracts when the
review's staged program reaches them.

## 70. Gate 1: the frame contract and reducer trace/replay (2026-09-26)

Implements the review's first delivery gate
([review-flat-temporal-aa-2026-09-26.md](review-flat-temporal-aa-2026-09-26.md)):
consolidate the FrameContract and the online/replay reducer without
changing output. The flat temporal reducer -- one streaming pass of
`flatRuntimeObserve` over FlatRuntimeDraw events -- now produces a named,
immutable per-frame artifact at the output-copy draw: `FlatFrameContract`
carries the assembled fixture records, the selection and the conflict
witness (flat_frame_contract.h), with a field-wise content hash over
exactly the decision-relevant fields. No decision changes: the online
driver consumes the identical FlatMonoFrame, and the sink
(FlatRuntimeContractSink) is a carrier, not a second selector.

Trace/replay (flat_trace.h): every reducer input event serializes into a
bounded, always-recording ring (four frames x 4096 events, frame-atomic, no
allocation on the draw path); the F10 audit arm dumps the complete frames
to `edvr_logs\traces\flat_trace_<frame>.bin`. The rig round-trips a
two-extent MonoFixture stream to an identical contract hash and replays
every committed trace in `tools/flat_temporal_test/traces/*.bin`, failing
the build if a recorded decision ever changes. The corpus starts empty:
next flights capture stock/EDHM menu and flight traces for commit.
Gates 2-5 (three-size routing, retirement, family contracts, composition
tests) are untouched; this gate exists to catch their regressions.

2026-09-26, same evening: the first capture's corpus replay failed 0/8 --
the gate working as intended. The v1 trace recorded only draw events,
while the prefix also mutates on the dispatch UAV guard, resource writes
(Copy/Map/Update), flatRuntimeUnknown and the camera overflow. v2 records
those as their own event kinds in sequence order, the dispatch guard now
shared by runtime and replay out of flat_runtime_model.h, and the magic
bumps to EDVRFTR2 (v1 captures rejected). Awaiting one recapture flight.

Same evening, second pass: the v2 captures still replayed 0/8, and the
rig's new `--trace-check` mode named the divergence -- decisions matched
(every frame selected) while hashes diverged, because capture() shares
prefix.sequence with draws (untraced camera captures left every later
draw's q low in replay) and the replay overwrote the traced write epochs
with its own counter. The trace now carries a camera-capture event kind,
replays the resolved write epochs verbatim, and the dump skips the
in-flight unsealed slot so captured frames are always complete. The two
v2 captures predate the fix; one more recapture validates.

Validated: the 21:15 captures (three sealed frames each from the EDHM menu
at 2496x1404 and from steady flight) replay byte-identical on every frame
-- stored and replayed contract hashes equal -- and pass the corpus gate:
2 files, 6/6 frames identical. Committed as the corpus's first entries:
tools/flat_temporal_test/traces/flat_trace_10806.bin (menu) and
flat_trace_16271.bin (flight). From here the corpus only grows: a frame
whose decision changes fails the build.

## 71. Second review cycle: observation transitions closed (2026-09-26)

[reviews/flat-temporal-main-review-2026-09-26.md](../reviews/flat-temporal-main-review-2026-09-26.md)
reviewed the section-68 redesign at `35c7afc6` and found two real
transition defects, both fixed in `a6ad1ace` and rig-tested as shared
predicates (flat_local_reject.h):

1. The on->off toggle left observation latched: a persistent refusal kept
   `observing` set while `partialWanted` flipped, so off's documented
   per-frame retry never resumed. `flatObservationToggle` now ends
   observation explicitly at the config read; history is already invalid
   from the observing frames, and off retries each frame from there.
2. The exit predicate cleared on any refusal-free frame, including empty
   ones with no handoff at all, and ignored the uncertain/foreign-work
   conditions. `flatObservationClears` now requires the copy draw's
   selector selecting through the contract observation (a positive witness)
   AND complete coverage (the same trio `phase.finish` uses).

The review's gate-1 guidance -- composed transitions from config through
observation, copy admission and next-frame history -- belongs to the staged
program's gate 4 (hooked end-to-end tests); today's rig covers the two
predicates. Also noted there: the observing copy return skips the old
spatial recovery for the one compromised frame a late refusal can leave;
the policy is "invalidate and observe", stated in section 68, and
recover() stays out of the mixed-phase path.

## 72. Gate 2 design: three sizes, retirement, negotiation (2026-09-26)

Per [the review](review-flat-temporal-aa-2026-09-26.md)'s gate 2. Today's
measured behaviour (flat_mono_resolve.cpp): DLAA refuses R != D outright
(`flat-dlaa-requires-native-render-size`); DLSS/FSR refuse R > D
(`flat-trained-resolve-cannot-downsample`); DLSS/FSR upscale R < D directly
(E = D, flown today: 2496x1404 balanced to 3840x2160); TAA evaluates at R
and composites to D with the bilinear minify. The design target is the
review's table; the deltas are the R > D route (DLAA at R, downsample E=R
to D, or native TAA at R) and the honesty rules (never silently disable
the requested backend, never hide a lower internal resolution).

Definitions the contract will carry explicitly: R = the game's render
size (tone target), D = the present size (output), E = the temporal
evaluation size. Route policy: DLSS/FSR upscale R < D evaluate at E = D;
native (R == D) evaluates at E = R = D; R > D evaluates at E = R with a
downsample stage E -> D (DLAA first, FSR Native AA pending its D3D11
port's support, TAA already conforms). TAA today is a fused display-grid
resolve at E = D sampling render-sized input (gate-2 review G2-2 -- the
route now reports that honestly); render-grid TAA at E = R with one
explicit R -> D conversion is the recorded design step, not yet built. The copy chain's own downsample
stays in place where it already maps R to D (the game does this at SS >
100% today); EDVR supplies a conversion only where the game's sampling
contract does not cover the replacement image's texel mapping.

Staged implementation:
1. (this section) Discovery: the contract reports R/E/D and the named
   route from the existing selection fields, logged on change; no
   behaviour change, no hash change (the corpus stands).
2. R > D: DLAA at R with the downsample qualified against the copy's
   texel mapping; FSR Native AA where the port allows it.
   SHIPPED 2026-09-26: the route table owns the size refusals
   (flatResolveRoute), NVIDIA supersampling evaluates DLAA at E = R and the
   game's copy downsamples to D (one final scaling step), FSR at R > D
   stays honestly refused until its port's Native AA qualifies. The
   preflight's drifted copy of the refusal rules is consolidated onto the
   route function. WARP-qualified: the rig's supersample case checks the
   backend receives E = R on both axes and the output view is render-sized.
   Same evening: the first SS > 100% flight refused every frame --
   flatContractKind's screen band capped sizes at <= D, so supersampled
   scene targets classified as none and no producer ever latched. The band
   now covers aspect-preserving sizes from 0.5x to 2x of output, with the
   conflicting-hdr guard as the loud backstop for full-res depthless
   intermediates; crops/ultrawide stay out of band for the lineage rework.
   2026-09-27 later: the band is now flatUniformScale -- a uniform render-to-
   output mapping within integer rounding of each axis (the exact bound of
   rounding a rational scale to pixels, NOT an arbitrary aspect tolerance).
   Rounded mappings like 1708x960 at 1366x768 admit; square shadow-like
   targets and non-uniform crops stay excluded (the last waits for the
   review's source-rectangle lineage). The selector's tone check uses the
   same rule. Rig covers native, rounded, mild-crop, sub-half, shadow-like,
   non-uniform, the 2x cap and past-cap.
   The 04:58 flight FLOWN: DLAA and TAA treat at 5760x3240 with the route
   lines naming each mode; FSR then engaged the same hour via Native AA --
   the 1.0x case of the same upscaler, evaluating at E = R with the game's
   copy downsampling, WARP-verified on both backends at render size.
3. Retirement: generation-based release of projection plans and
   classifier memos (finding 3's 32/64 cliffs), with the corpus replaying
   retirement boundaries.
   SHIPPED 2026-09-27: stale plans (a buffer generation advanced or its
   shadow gone) release their slots first, then the least-recently-used
   idle plan -- never a plan mid-scope, and each retired plan demotes its
   buffers' planRefs so promoted-forever buffers become evictable again;
   re-preflights demote before re-recording, so retargets are idempotent.
   The 64-pair classifier memo retires least-recently-seen instead of
   refusing ever after (coverage line memo-evictions=). WARP-qualified:
   40 topologies through a 32-plan cache with LRU retirement and a
   shadow-invalidated plan retiring stale ahead of any LRU victim.
4. Backend negotiation: the served-floor ladder (dlss_floor.h) already
   answers under-floor inputs on the VR door; the flat route gets the same
   query on extent changes, logged per route.
   SHIPPED 2026-09-27: flatDlssNegotiate names the effective treatment from
   the vendor's queried ranges -- serving mode and evaluation size, an
   under-floor input cut to the floor it reaches with the game's copy
   upsampling the rest (the VR door's rule), never a silent substitution.
   The negotiated E overrides the resolve frame's evaluation grid;
   requested vs effective treatment are both logged on every contract
   change. Rig-pinned over the flight's own ladder shape (served at the
   door output, ultra's point, proportional cuts, unanswered queries).
   FLOWN 2026-09-27 (09:55 Epic): the full ladder 1.0/0.5/0.65/1.5/1.0
   negotiated per change (floor performance at 0.5, quality at 0.65, DLAA
   supersample at 1.5) with ZERO fallback lines and continuous streaks.
   The first pass exposed a stale-override transition bug (the previous
   contract's negotiated E leaking into the next frame); the override is
   now gated to its exact contract signature.

Qualification matrix (the review's gate 2 tests): sub-native, native,
supersampled, odd sizes, crops, live extent changes without a restart,
each with the corpus growing one trace per cell.

## 73. Gate-1 review corrections: every copy outcome hashed (2026-09-26)

The third review pass
([reviews/flat-temporal-main-review-2026-09-26.md](../reviews/flat-temporal-main-review-2026-09-26.md),
the gate-1 entry) found three reproduced gaps in the corpus gate, all fixed
without a flight:

- G1-1: the contract recorded only record-producing copies, so early
  refusals (missing tone, uncertain input, conflicting HDR) and a duplicate
  copy's refusal all read `produced=false, hash=0` -- indistinguishable and
  unchecked. The contract now records EVERY copy draw's selection outcome
  in order (bounded at four; the first is the one the driver consumed),
  plus its conflict witness, and the hash covers them all. Fixture records
  come from the first record-assembling copy and are never overwritten.
- G1-2: the hash omitted the selected camera matrix (a camera[0][0]
  mutation passed), the selected output identity/extents and the aggregate
  instance counts. The hash now covers the full semantic selection and
  fixture, field-wise. Schema is explicit: trace magic EDVRFTR3, and the
  stored corpus hashes were regenerated from the same saved events by the
  rig's new `--trace-migrate` (same events, unchanged selector).
- G1-3: the corpus gate could pass on a missing directory, an unreadable
  entry or an empty set. It now fails the build on all three, and a
  manifest (tools/flat_temporal_test/traces/manifest.txt) pins the required
  scenarios with frame counts and build/mod provenance. `--trace-check`
  exits nonzero on any mismatch.

Smaller: the dump line reports emitted frames/events separately from
skipped (in-flight/truncated) slots and names a short write unusable; the
artifact's record camera aliases are rebased to the artifact's own bytes.
The rig gains the review's refusal matrix (missing tone, uncertain,
conflict-free duplicate, no-copy) and hash-mutation probes. Its synthetic
marker placement stays post-contract; discriminating pre-copy mutations are
the outcome tests above. What the trace does NOT certify -- mode, backend,
observation state, effective treatment, history and pixels -- is now stated
in the manifest's provenance; those belong to gates 2-4's GPU and flight
evidence.

## 74. Gate-2 review final pass: E keys the resource cache, bounded retirement (2026-09-27)

The [gate-2 review](../reviews/flat-temporal-gate2-review-2026-09-27.md)'s
final pass found two reproduced gaps, both fixed without a flight:

- F1: the negotiated evaluation size E was missing from the resolve's
  resource cache key. A cut E (step 4's under-floor negotiation) arriving
  after a default-E frame would reuse the default-sized allocation and the
  finish pass would fill only the E rectangle of it; and the preflight --
  which carries no frame -- allocated at the route's default E, so the
  first treated frame of a negotiated contract reallocated anyway. E now
  keys the cache alongside mode/R/D, the plan carries the same negotiated
  override the frame carries (gated to the exact contract signature), and
  the preflight allocates and verifies at that E.
- F2: the buffer-pressure path retired ONE plan and re-scanned; a buffer
  shared by two live plans keeps a nonzero planRefs after the first
  retirement, so nothing freed and the write refused. The pressure path
  now retires in a loop bounded by the plan-bank size, re-scanning after
  each retirement; the guards stand (never a promoted/mapped/pending
  buffer, never a non-idle plan).

WARP-qualified: the rig's cut probe walks default E -> cut E=24 -> default
on a 16x16-to-32x32 DLSS contract, checking the backend's observed grid,
the output view's size and the allocation count at each step, and that a
preflight carrying the cut lets the first treated frame hit the cache; the
shared-refs pressure case pins all 64 buffer slots twice over (32
four-binding plans, each buffer in exactly two, the second reference on
distinct slots so each plan is its own topology) and the 65th buffer
tracks only after the loop retires both plans pinning a shared buffer.
Corpus replay unchanged: 6/6 frames identical.

## 75. The 0.5x flicker: success-status presents reset history every frame (2026-09-27)

Sean's 0.5x report: the red canopy structure flickered between
stair-stepped and smooth on both upscalers, fine at 0.65x. His 12:33 A/B
bracketing an F8 cycle proved the treated path healthy -- EDVR DLSS at
0.5x smooth through a 1300-frame streak while stock 0.5x stair-stepped,
which is inherent to 1080p input. The 12:12 FSR flight's log held the
real defect: a 13-second episode where EVERY frame accepted a reset with
adapter reason no-previous (450/5s), history never accumulating, jitter
pinned at (0,0) as treated-zero-jitter, while refused froze and treated
kept counting. Mechanism: a treated frame always sets the adapter's
previous at frame end, so only reset() between frames produces
every-frame no-previous; refuse() is excluded (its counter froze); and
the jitter collapse names the survivor -- phase.finish received
hr != S_OK every frame, pinning previousAcceptedValid false and the
phase at zero. flatRuntimePresent gated the history reset and the phase
finish on hr != S_OK, so any success-status Present (DXGI occlusion and
friends, or a chained mod's status) reset temporal history every frame
for as long as the status persisted.
SHIPPED 2026-09-27: both gates now use FAILED(hr)/SUCCEEDED(hr) --
success statuses no longer reset history or stall the phase -- and the
present value is logged (8/session, present-not-ok= in the 5s adapter
line) so the next flight names the exact status. CONFIRMING FLIGHT
REQUIRED: hit the condition (overlay/alt-tab/whatever produces it) and
verify present-not-ok names the value with NO no-previous storm through
it. The stair-stepped 0.5x look WITHOUT a storm is inherent (stock shows
it too); FSR's resolve of the canopy at 0.5x during healthy accumulation
is not yet separately qualified.

Addendum, same day: the 13:00 flight (g0b56952d) ran storm-free --
present-not-ok=0 in every window, treated streaks past 4000 -- and the
canopy flicker persisted on both backends, so the storm and the
structure-localized flicker are separate phenomena. The 12:32 F10
capture (frames 36808/36823/36838, healthy 0.5x DLSS) shows the
mechanism directly: the struts are ~1 render pixel at 1080p, and their
fringe pixels oscillate between frames at |delta|~68/255 while their
cores sit at ~2 -- jitter moves the sub-pixel structure on and off texel
centers each phase, the input to either upscaler oscillates there, and
the temporal logic flickers on exactly those pixels. At 0.65x the struts
are ~1.7 px, mostly covered every phase, hence stable.
ruled out: rejection-mask flip-flop, because rejection is stable on 97%
of the structure's pixels across the three frames.
ruled out: depth/motion corruption, because depth reads 0.0009+-0.0003
stable and the menu camera is static.
ruled out: a backend defect, because both networks flicker identically
while the shared input oscillates.
CLOSED (live, 2026-09-27 evening): the parked jitter-off test confirms
the family -- HUD and canopy scintillation both die with jitter off
while reprojection keeps running; see section 76 session 3.
No threshold/clamp compensation: the oscillation is real coverage
signal. 0.5x's price is flicker or softness; jitter-off is the manual
escape hatch, not a default.

## 76. Gate-2 qualification flight matrix: plan and corpus status (2026-09-27)

The review's remaining boundary: the corpus certified one size pairing
(0.65x DLSS, menu and flight); odd sizes, repeated mid-session changes,
the other backends and the mod matrix are open. Crops stay deferred on
the source-rectangle lineage; backend-failure forcing stays with the
rigs. One trace per cell, admitted to the corpus with its manifest line;
each F10 seals the three frames before the arm and dumps
flat_trace_<frame>.bin into the game dir's edvr_logs\traces.

Corpus today: edhm-menu-ss065-dlss, edhm-flight-ss065-dlss (2026-09-26),
plus edhm-menu-ss050-dlss admitted 2026-09-27 from the 12:32 flight.

Session script (one sitting, EDHM baseline, Epic install, start DLSS at
SS 1.0; each step: wait ~20 s of treated frames, then F10):

1. Main menu, SS 1.0, DLSS -> F10 (native menu).
2. In flight, SS 1.0, DLSS -> F10 (native flight).
3. SS 1.5, DLSS -> F10 (dlss-as-dlaa supersample).
4. F8 to FSR, SS 0.5 -> F10 (FSR sub-native).
5. SS 1.5, FSR -> F10 (FSR Native AA supersample).
6. F8 to TAA, SS 1.0 -> F10 (TAA display-grid native).
7. Output resolution 3840x2160 -> 2560x1440 mid-session, SS 0.65 -> F10
   (odd pairing AND a live extent change without restart).
8. Resolution back to 3840x2160, SS 1.0: no F10 needed; the runtime line
   showing the streak resuming closes the repeated-changes cell.

Per cell: one "flat route:" line names the effective treatment (the
negotiation line joins it for sub-native DLSS); treated streaks rebuild
within seconds of each change; no reset events beyond the change itself;
the canopy scintillation at 0.5x is expected and recorded (section 75),
not a blocker. After the session: --trace-check every new dump, admit
one trace per cell with its manifest provenance line, full build (the
corpus gate must print every file replaying identical), push. The
EDHM-off stock matrix is a separate sitting (chain unlink, restart).

Session 1 (2026-09-27 13:24, g0b56952d): menu cells only; the flight
segment hit a NEW unreciped pair. Banked: edhm-menu-native-dlss (streak
6000+ at capture). Eight seconds after entering flight, every frame
began refusing locally on VS 24214E7C45496BE0 / PS EC998602427115F3
(unknown-scene-projection-recipe, ~450 draws/5s window, every frame) and
the runtime sat in observation for the rest of the session -- the
section-68 design working as intended: zero treated, zero refusal-count
churn, no reset storm, jitter parked at zero. All seven dumps replay
identical under --trace-check, but the six post-observation ones
(46463/48357/51105/53342/55208/57533) certify selection during local
refusal, not treated cells; they stay unadmitted. No SS or resolution
change was ever applied (no route lines past the initial native), so the
supersample, sub-native-backend, odd-size and repeated-change cells all
remain open. The refusing pair's bytecode IS on disk (shader capture:
vs_24214E7C45496BE0.dxbc, ps_EC998602427115F3.dxbc) -- recipe it and the
flight cells can run. CORRECTION (same evening): an earlier version of
this note claimed the unknown-projection capture reported
distinct-pairs=0 all session and opened an instrumentation question.
That was a log-sampling error on my part -- every window I had grepped
ended before 13:26:36. The log shows the automatic post-audit path
taking the pair at 13:26:36 (frame=45891, trigger=automatic,
bytecode-stages-saved=2), the F10 rearm re-taking it under the audit
trigger at 13:26:42, and distinct-pairs=1 stable to session end. The
capture pipeline worked exactly as designed; those saved bytes are the
.dxbc the recipe below was reviewed from.

Recipe SHIPPED 2026-09-27: VS 24214E7C45496BE0 is the radar local-key
marker VS, already vetted at cb2 ForwardColumns 8 against the stock
companion; EC998602 is the same marker recoloured through EDHM's t120
config tree (same input semantics, colour-only delta, no projection or
depth consumer), admitted as the exact companion. Not one of the
coriolis arc's named seam suspects (BCF75CEA37060EAE / 2F924695596C8195
at SV_Target6) -- adjacent family, new companion only. Rig pins the
pair. The flight cells now wait on a re-fly, not on code.

Session 2 (2026-09-27 13:56, g7a0415b2, in flight): the recipe holds --
no observation episode, no local refusal beyond three one-draw transient
projection-preparation-refused lines, present-not-ok=0 session-wide.
Banked five cells, each captured with a rebuilt treated streak:
edhm-flight-ss150-dlss (dlss-as-dlaa-supersample), edhm-flight-ss150-fsr
(fsr-native-aa-supersample), edhm-flight-ss050-fsr (trained-upscale),
edhm-flight-ss150-taa (display-grid down), edhm-flight-native-taa.
Repeated contract changes rebuilt streaks within seconds every time --
the repeated-changes cell stands on this evidence. Corpus is 9 files,
27/27 replay identical. Still open: native DLSS flight, 0.5 DLSS flight,
the odd-size/resolution-change cell (no route ever left 4K), and the
EDHM-off stock matrix.

Session 3 (2026-09-27 14:29, g7a0415b2, STOCK -- EDHM disabled): banked
stock-flight-res2560x1440-dlss (the odd-size cell: borderless 2560x1440
render under the 4K backbuffer, negotiated quality at E=D),
stock-flight-ss050-dlss and stock-onfoot-station-ss050-dlss. Corpus is
12 files, 36/36 replay identical. Sean reported cockpit HUD elements
"swimming sometimes with motion" at 0.5x and station flicker; the 0.5x
DLSS cockpit pixel capture (frames 52245/52260/52275) answers the
machinery: HUD panel pixels carry cockpit-consistent near vectors
(matching the dashboard, not space), the world-anchored local-key marker
carries its far anchor's vector, rejection behaves on animated hologram
content, and template-tracked displacement shows the panels locked to
the cockpit geometry -- the reprojection input is correct in this
capture. Remaining candidates for the visible swimming: the section-75
sub-pixel scintillation family on 1px hologram lines at 1080p (same
resolution floor as the canopy, expected at 0.5x), or a fast-motion
vector failure this slow-motion capture cannot see. The discriminator is
the parked jitter-off test: scintillation dies with jitter off; a true
vector error would persist. CLOSED 2026-09-27 evening: Sean flew 0.5x
with experimental.temporal_aa_jitter=off -- the HUD went low-res (the
expected trade) and did NOT smear/swim with motion. The resolve still
reprojects by its motion vectors with jitter off, so a vector error
would have persisted; it did not. The swimming is the section-75
scintillation family, confirmed twice over (canopy probe, live HUD
test); the vectors are right (cockpit capture, locked to the dashboard).
0.5x's honest price is flicker or softness; jitter-off is the manual
escape hatch, not a default.

## 77. rc-since-rc2 review: all six findings addressed (2026-09-27)

Per [the review](../reviews/rc-since-rc2-review-2026-09-27.md)'s
F1-F6, all fixed without a flight:

- F1 (generic admission accepts a camera-dependent PS): the classifier
  now refuses a texture coordinate whose used components carry vector-dot
  (coefKind=2) terms from >=2 distinct rows of one cb -- the separate-U/V
  idiom (U=dp4(v,rowA), V=dp4(v,rowB)), reason multi-row-texcoord.
  Scoped to vector-dot terms so the rect-filter idiom (scale/offset
  constants) still passes; the inert stock filter ps_F0BAE053476F8730
  verifies Clean. Rig: synthetic two-row dp4 sample refuses, single-row
  control passes. FOLLOW-UP (same evening): the reviewer's sqrt and
  divide escape variants -- a fetch overwriting its own coordinate
  register (sample into r0) and the projective divide (uv/w) -- erased
  the row evidence before the first check read it. The coordinate's row
  provenance is now evaluated at each fetch's POINT OF USE inside
  buildForms (facts.texCoordMultiRowDot), and a divide with a term-free
  denominator forwards the numerator's vector-dot terms. The reviewer's
  own probe confirms matrix, matrix_sqrt and matrix_div all refuse as
  multi-row-texcoord; the rect-filter fixture still passes.
- F2 (classifier misses legal SV_Depth): the plain dcl_output form's
  depth operand (oDepth/oDepthGE/oDepthLE, operand types 12/38/39) now
  sets depthOutput, alongside the Sgv/Siv system-value path. Rig:
  hand-assembled binaries for all three refuse as depth-output; a plain
  colour output stays Clean.
- F3 (12 bytes read past the 4-byte frame stamp): the 16-byte stamp cell
  now uploads an initialized uint32_t[4]; the S2 lifecycle test checks
  the whole copied float4 (frame, 0, 0, 0).
- F4 (after-UI capture bypasses exclusions): the retry preserves the
  original decision's two exclusions before attempting the take -- the
  ui_depth shader exclusion and the held world-screen identity
  (panel-sized SRV while the screen shows the world) -- via
  uiLayerAfterWritePreserved, facts gathered at the vscreen call site.
  VR-only path (fix.ui_quality, off by default then, 100 since 2026-09-29);
  flat does not use it.
  Rig: the test traverses the original kWorldScreen decision and the
  retry gate together.
- F5 (negotiated E one frame late): negotiation now completes BEFORE the
  frame's and the plan's eval override are assigned (flatNegotiatedEval,
  called post-negotiation in both places), so the first frame of an
  under-floor contract resolves and preflights at the cut E -- no
  default-E refusal, no next-frame reallocation. E is also part of
  sameResolvePlan, so readiness tracks the effective plan. Rig: the
  gate's truth table (flat_negotiated_eval_tests.h).
- F6 (276-row captures crash the analyzer): load_manifest refuses
  non-reset captures whose scene constants lack the freshness-stamp row
  (a named CaptureError), reset captures on the legacy layout stay
  accepted, and the engine guards the read with a named failure. Rig:
  both boundary fixtures in flat_pixels --self-test.

F7 (the updated pass's new P1): the sampled VR PS-shadow probe could
adopt EDVR's own installed substitution as game state -- the probe
assumed EDVR shaders hash zero, but the production hook registers the
generated patch's nonzero hash, so a sample wrote the patch into the
game shadow, broke the generation restore() compares by, and lost the
original's identity (the frame boundary then had nothing to restore
with). The probe now skips the currently installed patch by pointer
identity before any registry lookup -- the shadow keeps the game's
original and the saved generation. Lifecycle rig (gate-executed):
substitution survives a sample with shadow and generation untouched,
the frame boundary restores the original, and a genuine bypass-bound
game shader still heals (engine_velocity_test 1162 checks). The pass's
four performance items (inverse stamp decode, stage-verdict cache,
exact-plan prepare reuse, camera hash at capture) are assessed
optimization candidates with equivalence/measurement plans, not RC
blockers; they wait on their own equivalence tests and a GPU timing
flight before any source change.

Full build green: mono resolve PASS, collector policy PASS, corpus
36/36, engine_velocity 1157 checks, ui_quality 254 checks,
openxr_shutdown 63 checks, flat_pixels self-test passed. One transient
openxr_shutdown WARP-binding flake in an earlier attempt did not
reproduce in the two following runs.

## 78. Ship-population coverage: the ten-pair triage (2026-09-28)

The supporter's never-starting report
([reviews/flat-aa-user-log-20260928-030237.md](../reviews/flat-aa-user-log-20260928-030237.md))
and Sean's same-evening reproduction (Caspian Explorer; the fleet
carrier also refused) are the same defect class: known VS families with
NEW pixel-shader companions, refusing generically every frame so the
runtime holds observation forever (calls=0). Bytecode review of the
session's ten refusing pairs (18 of 20 stages captured locally):
every PS is colour-only by the established vetting -- texture
coordinates from varyings or scalar-built grids, cb1 reads only at
61/90/210/227..254 (lighting) and 277..279 (orientation dp3), never the
270..273 clip rows, no depth output. The multi-row-temp verdicts are
paint-layer/material blends, indexable-temp is array lighting, resinfo
is an atlas-dimension query, and ps_A9975F91040B0BCD is the decal
projective-depth family exactly as the already-reciped 0A298DE7/
D8FCE3CE cases. vs_A47A3315FFF5E2E4 is a partial-z ForwardDp4(0,4);
vs_ACE405F428C17EF6 and vs_72BDD292154158AD project ForwardColumns
270..273 behind a cb0[9..11] local pre-transform (the ce715126 block's
local-transform note); vs_2BB766C168B450A2 is columns 270..273 with a
harmless resinfo atlas query. Nine pairs reciped and rig-pinned (census
46); vs_C7FA0C0F5DD49180's blob was never captured, so its pair stays
refused until a capture supplies it. The review's follow-up notes the
architectural direction: exact recipes are the current mechanism, ship
diversity is regression coverage, an upstream flat camera/projection
hook is the proposed long-term investigation (unvalidated, not
started), and the refusal is never to be bypassed.

## 79. Two rc.4 users: every frame refused, passes between tone and copy (2026-09-30)

Two flat-profile users on v0.18.0-rc.4 (game build 332841, the same as
Sean's) reported DLSS "not activating". Their logs agree: `treated=0` in
every window, each frame refused with `no-known-tone-pass`, the DLSS
renderer never called (`calls=0`), the camera path `warming` throughout
(`injected=0`). The discovery's chain dump names the post chain (menu
frames only: its per-record detail fired on the first two dumps):

| | tone pass | between tone and copy | settings |
|---|---|---|---|
| Sean (treated) | known VS and PS | nothing | game AA off |
| user 1: 4K, `AAMode` 4, bloom/DoF/AO off, one unidentified chained proxy | known VS F9CFC798F21E9AEA, new PS 6E83D02E7422C5BA | 03D186CE0EC031E3/BAB75803059C271D, then 98E6F9986FDC9A53/4168985B52C5D7C4 (fmt 27) | game AA on |
| user 2: EDHM chained, 3840x2160 scene to a 2560x1440 output, DoF 2, bloom 3, AO 3 | known VS, known DoF PS DE65BFFF2F12ECC6 | 20F383BBAC05C031/5AA08A96E3C14B10 (316 B), then 20F383BBAC05C031/2375CCCCBBFE7A4D (5 KB), one fmt-27 target | bloom 3 |

The selector takes the tone pass only as the writer of the final copy's
source and only from its hash list (`flat_mono_frame.h`, the tone search
after the copy), so both refuse. Confirmed by the users, relayed by Sean:
user 1 was fixed by turning the game's AA off, user 2 by turning bloom
off. Blur is fine; depth of field was reported to break it too, so the
fault depends on the combination of post-processing passes and not on one
setting (Sean's all-maxed test, with DoF and bloom on, was treated).

Also measured: with a temporal mode selected and every frame refused,
user 1 presented 60-61 fps against 130-270 with AA off. EDVR's GPU census
read about 0.07 ms a frame, but that census cannot price the flat path: it
times neither the flat resolver nor engine motion's overlay copy and
substituted draws, so the reading proves nothing either way about where
the loss is. User 3's Task Manager (i5-10400F, RTX 3050 at 31% while DLAA
ran, about one busy thread; section 80) later pointed at the CPU and
driver side, and the census section 80 built settles it. User 2 stayed
near a 120 fps cap. The warning anti-aliasing.md planned ("Elite's
anti-aliasing appears to be on") was never built, so nothing told either
user why.

Both users' machines saved the unknown passes' bytecode (`flat producer
shader ... existing=1`) and an F10 trace (`flat_trace_36244.bin`,
`flat_trace_5481.bin`); the log bundler left both out (a separate task).

Open, Sean to decide: treat the copy's source when every pass between the
tone pass and the copy is a plain image pass (versus admitting these
hashes one by one); say in F8 and the log when a frame is refused and why;
stand the per-draw work down while every frame is refused. Section 80:
the last two are built; the first is deferred by decision, and the
selector is not relaxed.

## 80. Refused frames stand down, F8 says why, a flat CPU census (2026-09-29)

Branch `claude/flat-refusal-warning`, cut from main `0ab66bd9`. Built and
rig-tested; flown once on Epic 2026-09-30 (readout in the note at the end).
Five code commits: the stand-down `6a033956`, the F8 warning `a3efa164`, the
census `bc382f63`, engine motion's timing line `734264cd`, the bounded camera
witness `2f87d15d`.

**Third field record (user 3).** rc.4, flat, 1920x1080 exclusive
fullscreen, 60 fps cap, no chained mod, game AA, bloom and DoF all off.
His frames ARE treated (counts up to 373 in the logs, reason
`treated-jittered`, camera injections 2.6-4k per 5 s window), yet 7-13
treated fps against 52-56 presented with AA off. EDVR's own GPU census
reads 0.1-0.15 ms a frame; his Task Manager (i5-10400F, RTX 3050 8 GB)
showed the GPU at 31% while DLAA ran and about one busy thread. So this is
not the refused-frames loss of users 1 and 2, and the GPU census that
priced 0.1 ms cannot see either half of the flat path's cost. Hence the
CPU and GPU census below.

**Stand-down** (`flat_standdown.h`, pure, `applyWork` in the runtime). The
runtime ran its whole per-draw and per-call pipeline for frames it then
refused. Now a run of frames whose only verdict is a chain-shape refusal
(no, ambiguous or invalid tone pass; no, ambiguous or invalid output copy;
broken lineage; wrong order; a watched frame that reached no recognised
copy counts as no-known-output-copy) lasting 5 s stands the work down. Any
frame with a selecting copy draw is treatable whatever another copy said;
a transient refusal (warming, reset, truncation, missing HDR) breaks the
run; unwatched frames do not count. While stood down every frame is
Paused except one whole Probe frame every 1.5 s that runs the contract
observation only (the online prefix model and the selector, the same code
and inputs). A probe the selector selects resumes the work at once with no
treated frame needed first, so warm-up cannot deadlock (a rig case replays
supported, unsupported, supported and asserts it). The AA mode changing, a
swap-chain or device reset, and an F10 audit wake it. Paused: coverage
classification and legacy projection readiness, the constant-buffer
shadows on every Map/Unmap/Update, the camera-write witness, the camera
refresh hook (its relay gate closes only when no camera holds an injected
phase, and never reopens a hook that stood down for good), engine motion's
hooks and substitution, the discovery observers, the draw-capture and
audit probes. Kept: the O(1) trackers (viewport, constant-buffer binds,
ClearState), so a resume starts from true state. The trace ring is not
rotated on Paused frames. A session whose frames are selected never leaves
Full and runs the code it always ran. Three lines, all prefixed
`flat stand-down:`: `entered at frame=N: every frame for 5.0 s (M frames)
was refused for <reason>, none treated; paused: ...`, `resumed at frame=N
after S s stood down (P probes): a probe frame's chain was recognised and
selected; all work restarts` (or `ended ...: <cause>` for a wake), and a
`still stood down ...` reminder every 30 s. Absent from a log means it
never stood down.

**F8 warning** (`flat_elite_settings.h`, `menu.cpp`). Shown only while a
temporal mode is selected AND the work is stood down for a chain-shape
refusal that found an output copy (as first built it also warned after a 2 s
structural run and for no-known-output-copy; both faults are fixed, see the
2026-09-30 note); treated frames, transient refusals and any refusal shorter
than the 5 s stand-down trigger show nothing, so a settings-only rule cannot
fire on a combination that works. The words, as
note lines under the rows, wrapped to the card: `<mode> is not active:
Elite's post-processing is not recognised.` Then, from Elite's own files
under `%LOCALAPPDATA%\Frontier Developments\Elite Dangerous\Options\
Graphics` (`Settings.xml` `<PresetName>`; for Custom, the highest
`Custom.<major>.<minor>.fxcfg` present, which is what the game reads, with
`<AAMode>`, `<BloomQuality>`, `<DOFEnabled>`, 0 for off): `Turn off in
Elite's graphics options: Anti-aliasing, Bloom, Depth of field` (only the
ones on); for Custom with none on, an unreadable file or unknown settings:
`Please send your logs (F10 in the cockpit, then the installer's log
bundle).`; for any other preset, named and not parsed: `Elite's <Preset>
graphics preset may turn on Anti-aliasing, Bloom or Depth of field. Turn
them off in Elite's graphics options.` The folder is read when the panel
opens, when the warning becomes wanted, and at most every 2 s while either
holds, the two files only when a write time, the chosen file or the file
set changed; a treated session with the panel closed never touches the
disk. Log: `flat settings: Elite graphics preset=... file=... AAMode=...
BloomQuality=... DOFEnabled=...` once per read change, `flat settings
warning: shown|changed (mode=DLSS, frames refused for no-known-tone-pass
[, work stood down]): <the words>` and `... hidden ...` per change, at most
24 a session. No new ini keys. The installer's log bundler now takes the
folder from the same header (`src/common/elite_graphics_folder.h`). Left
alone: `eliteHmdMultiplier` (`device_hook.cpp`) picks the newest `.fxcfg` of
ANY preset by write time, which is not the file the warning reads; user 3
had `Custom.4.0` to `4.4` side by side. Since 2026-09-30 (section 81): with
the HDR route active the Bloom and Depth of field advice is dropped, and
when frames are refused, the route's key is auto and the game renders below
the output (Elite's supersampling under 1.0, from the route's own measured
sizes) a third paragraph follows the advice: `Supersampling is below 1.0.
At 1.0 or above, EDVR anti-aliases before bloom and depth of field, so they
no longer block it. Raising it costs GPU time.` The log line's brackets gain
`, HDR route active` or `, supersampling below 1.0 (render WxH, output
WxH)`, and it carries every paragraph.

**The census** (`flat_cpu.h`; modelled on `engine_motion_cpu.h`, which the
flat menu tick never reaches). One `flat cpu 5s:` line every 5 s while a
temporal mode is selected, zeros included, continuation lines
(`flat cpu 5s (cont.):`, none over 1090 characters) when it does not fit:
`frames=N (stood down M) present p50 X ms (p95 Y); EDVR per frame total a ms
= other ... + contract reduction ... + copy checks + camera rows + trace
ring + resource lookup + coverage + projection readiness + cb shadows +
camera witness + engine motion draw wrapper + resolve + backend + discovery
+ state trackers + camera inject + engine motion hooks`, each `b ms (calls
c)` per frame on the render thread, every call clocked, exclusive of
nested families so they partition the time. Then `other threads (clocked
on K of N frames, one in 32; thread-ms per clocked frame)`: camera inject
and engine motion's emit, rigid emit, copier, merge, clear, jobs, builder,
tees, the evaluator relays counted; `camera witness U us/write (W
writes)`; `engine motion wrapper D3D calls C/frame over D substituted
draws/frame`; `GPU frame p50 / p95 (first game draw to Present; n timed, s
skipped, i invalid)` and `GPU resolve` (the resolver's dispatches plus
backend, two hooks in `flat_mono_resolve`), read back without waiting,
`-` when nothing was timed; the calibrated clock floor and what the clocks
themselves cost the render thread. Engine motion's own instrument is driven
by the census here (its draw side is inside the wrapper span, not counted
twice). Two small fixes rode with it: engine motion's draw-side CPU and
driver-call figures moved off the truncated `movers joined` line onto
`engine motion: draw side over ...` (C5), and the camera-write witness's
stack walk, which ran on every camera write for the whole session, stops
after 32 walks in a row that learn nothing or 128 in all and is re-armed
by F10 (C1).

**What a flight should show.** Refused shape (a Custom preset with AA on,
DLSS selected): `flat stand-down: entered` about 5 s into the scene; the
census reporting `stood down` frames and a per-frame total near the
trackers alone; present p50 back toward the AA-off cadence (user 1: 60 fps
to 130+); the F8 note lines and `flat settings warning: shown`; turning AA
off in game, `flat stand-down: resumed` within about 2 s, treated frames
back, the warning `hidden`. Treated shape (user 3's, or Sean's rig with
DLAA): read `EDVR per frame total` against the gap between the treated and
the AA-off frame time. If the total is most of the gap, the family names
the cost (the wrapper's D3D calls per frame say how many driver round
trips it is; `camera witness` should fall away after the bounded walks);
if `GPU frame` p50 is near the present p50, the frame is GPU-bound and
`GPU resolve` says how much of it is ours; if neither, the loss is not in
EDVR's own hooks and the census has ruled that half out. Not in any
family: the shared hook layer's per-call shadow updates on every state
setter. The census's clocks cost about 33 ns a scope on the build machine;
the line prints the floor and the price.

**Decided, not built.** Treating the copy's source when every pass
between tone and copy is a plain image pass (section 79's first open
item) is deferred, and the selector is not relaxed.

**Environment.** Flat profile only; no VR runtime, headset or per-eye
size is involved, and none of this touches the VR path. D3D11 immediate
context; the census needs the owner thread's Present. Fixed sizes: 64
thread slots for the census (later threads share one, approximate), 2048
samples per window for the percentiles, four whole-frame and two resolver
GPU timers, the witness's 16 site slots.

**2026-09-30 note: flight 052916 (Epic, build `ce6d511a`, about 1,000 draws a
frame).** Stand-down: startup entered for no-known-output-copy and resumed
after 27.2 s (18 probes); Elite AA on (`AAMode=4`) entered for no-known-tone-
pass with the right F8 text and resumed 13.5 s after AA went off. The witness
stopped after 33 walks. DLSS treated: EDVR 0.89 ms a frame (the clocks cost
0.40 ms, 0.19 ms of it in the total); largest part the draw wrapper, 0.21 ms,
3,228 D3D calls a frame over 254 substituted draws. GPU frame p50 2.13 ms (the
DLSS resolve 0.40 ms), present p50 2.15 ms: GPU-bound. Stood down: EDVR 0.009
ms, present 1.21 ms. So on Sean's CPU the per-draw work is small, and user 3's
~60 ms a frame is not this work on a slower CPU (he alone runs exclusive
fullscreen at a 60 fps cap with RTSS). Two F8 faults, fixed on
`claude/flat-warning-fix`, unflown: it warned for no-known-output-copy at startup
(05:29:20) and flickered (shown 05:30:09.904, hidden 05:30:10.378, after a 2 s
run); it now follows the stand-down and never warns for no-known-output-copy.

**2026-09-30 note: flight 053745, on foot in a hangar (Epic, `ce6d511a`, EDHM
chained, 1080p render, frame cap off, one spot).** Borderless: AA off about
4.0 ms a frame (234-274 fps), DLSS present p50 8.5-8.9 ms. Exclusive
fullscreen: DLSS 7.0-7.3 ms, AA off about 3.0 ms (331-351 fps).

- ruled out: exclusive fullscreen as user 3's collapse, because DLSS adds the
  same 4.2-4.7 ms in both modes and exclusive is the faster of the two.
- Census on foot: EDVR's render thread 3.5-3.8 ms a frame, of which the
  census's own clocks put about 0.8 ms in and cost about 1.7 ms in all (about
  50,000 scopes a frame), so EDVR's own work is about 2.9 ms. The draw
  wrapper 0.89 ms (13,549 D3D calls a frame over 1,130 substituted draws),
  hook entry ("other") 0.94 ms over about 10,000 calls, camera rows 0.44 ms
  over about 5,000, contract reduction, coverage, copy checks and the trace
  ring about 0.2 ms each. On job threads the engine clear and merge observers
  take about 2.2 ms a frame. GPU frame p50 5.9-7.2 ms, the DLSS resolve
  0.40-0.86 ms.
- So on foot EDVR costs about 3 ms of Sean's render thread a frame, four
  times the 0.9 ms of flight 052916's scene: worth cutting for everyone, and
  the census must be sampled before a release. User 3's roughly 50 ms a
  frame is still unexplained by it (RTSS, or their system); next are user 3's
  RTSS-off test and a census build on their machine.

**2026-09-30 note: the CPU cuts, and a fourth field record (branch
`claude/flat-cpu-cuts`, cut from main `fef6d55a`; built and rig-tested, NOT
flown).** Flight 053745's census priced the flat path's CPU work at about
3 ms a frame on Sean's rig: the draw wrapper's 13,549 D3D calls a frame, the
camera-row lookup's 0.44 ms over about 5,000 calls, and the clocks' own 1.7
ms. Sean approved five pieces; each is its own commit.

- **A, the census is sampled** (`b1ecc903`). The render thread is clocked on
  one frame in 16, exactly one in each block of 16 at a random place, and
  the other threads' one-in-32 sampled frames are always among them. The line
  says "render thread clocked on K of N frames, one in 16" and its per-frame
  figures divide by K; present p50 and p95 still come from every frame, the
  GPU timestamps are unchanged, and the self-cost line prices the sampled
  clocks. A rig compares a sampled window with a fully clocked one on the same
  workload.
- **B, the camera-row lookup is kept** (`513bb41a`, `flat_camera_table.h`).
  The draw's answer (rows, hash, epoch, sequence) is kept until the b1
  identity, its binding generation, the frame or the table changes, and every
  table mutation (claim, invalidate, map, unmap, capture, invalidate-all,
  frame, clear) bumps the table's generation. The record the draw fills is
  byte-identical to the fresh lookup's (a rig replays random scripts of
  writes, rebinds and captures against the old algorithm); the search is
  timed only when it is made afresh.
- **C, engine motion's substitution stays bound across producer draws**
  (`13b1b8ee`, `flat_substitution.h`). The wrapper read the game's eight
  targets, bound MRT6, the derived blend and the patched pixel shader, drew,
  and restored all of it around EVERY producer draw: eleven context calls a
  draw at least, and a cache reset that sent the next draw through the slow
  half. Now the state stays bound, as VR's always has, and the game's is put
  back once, before anything that could observe or depend on it: any hooked
  draw that is not a substituted producer draw, dispatch, clear, copy,
  resolve, command list, an OMSetRenderTargetsAndUnorderedAccessViews that
  keeps the targets, and the Present flush; ClearState and a resize forget
  without touching a context that may be gone. A game setter of the same
  state needs no event (the shadow's generations say so). What it cannot see
  is a game Get*: none is hooked, so a Get between two producer draws reads
  EDVR's state, exactly as it can under VR; every other hooked call restores
  first instead. A diagnostic capture, or the overlay guard's private t3,
  restores after each draw as before. Rig, on WARP with the context's methods
  counted on its own vtable: 40 consecutive draws cost 440 calls before and 3
  now (99% fewer; 43 with the game changing its pixel shader before each);
  every game call sees exactly the game's state and every producer draw
  EDVR's; each dropped restore, and 16 mutations of the engine side, are
  caught. VR is not touched.
- **The query cut** (`515af954`, `flat_query_cut.h`). With the state kept,
  the questions left were the coverage classification's (the depth view, and
  the actual shaders of draws whose projection needs no patch) and the
  wrapper's per run (the game's targets, blend state and the runtime's
  acceptance of MRT6). Each is now answered from what the runtime already
  tracks (the shadow's depth resource and shader hashes; the blend state and
  the saved target set kept under the generations of the game's bindings; the
  read-back once per binding), and one frame in 64 puts up to four questions
  per state to the context as well and compares. A disagreement is counted,
  that state asks the context for the rest of the session, and one log line
  names it; the flat CPU line reports answers a frame, checks and wrong per
  state. Not cut: qualifyProjection's shader, viewport and constant-buffer
  reads run only for an F10 audit or the legacy route (it returns first under
  Upstream ownership), and those two still put the game's state back first;
  the eight-target read stays once per binding (a restore needs views it
  owns; a shadow pointer can dangle after an unhooked unbind); the UAV check
  stays a read per run. Rig: 40 runs of one draw cost 394 calls asking, 276
  answering (7 a run, not 10); a blend state or target set changed by a real
  call nothing hooked is found on a checking frame and is NOT seen between
  checks, which is what one frame in 64 bounds. 27 engine, 15 policy/read/
  census and 5 wiring mutations are caught.
- **F8 note** (`a82fac56`, `flat_wrapper_note.h`). When the hook-mode probe
  finds the context's methods outside Windows' d3d11.dll and the session
  hooks in place, the panel says, while a temporal mode is selected, "<file>
  handles every graphics call (likely ReShade): anti-aliasing costs more
  frame time with it."; the file is the module backing most of the table
  (`vtableDominantOtherModule`). Never for a live copy (EDHM and 3Dmigoto sit
  at 96 of 96), never for a forced mode, no new key; one log line at the probe
  and one when the note is first drawn.

**Fourth field record (user 4).** rc.4, flat, Ryzen 7 9700X, Radeon RX 9070
XT (AMD: NGX refuses, the chain ran FSR), 5120x1440 exclusive fullscreen at
144 Hz, Elite's limiter on at 60, SSAA 0.85, DoF on. Frames refused for
no-known-tone-pass held 55-60 fps; from 14:25:39, treated and jittered,
20-30 fps, about 23 ms a frame more, on a CPU at least as fast as Sean's.

- ruled out: AMD Fluid Motion Frames 2.1 as the cause, because turning it off
  left the drop.
- CONFIRMED: ReShade. The log has `context hook mode: InPlace -- 0 of 96
  sampled vtable entries are inside Windows' d3d11.dll`, DrawIndexed at
  `...\dxgi.dll+0xFF790`, and with ReShade disabled the treated frame rate
  came back. A wrapper makes every one of EDVR's per-frame D3D calls dear and
  the treated path makes thousands a frame, so for anyone behind a wrapper
  the NUMBER of calls is the lever, not only their CPU time: C and the query
  cut are the answer to it, and the F8 note says so on screen.
- Shared with user 3, untested by Sean: exclusive fullscreen with Elite's
  limiter at 60 (Sean flew exclusive uncapped, no collapse). Next test:
  limiter on, exclusive, in the hangar. User 3's context is Windows' own
  (LiveCopy, 96 of 96), so ReShade does not explain his ~50 ms; the RTSS-off
  test and a census build on his machine stay next.
- His EDHM stopped loading because `real_dll` in `edvr-flat.ini` was changed
  to `d3d11.dll` between two launches by something outside EDVR; EDVR refuses
  to chain to itself and forwarded to the system d3d11. Proposed, NOT built:
  fall back to the installer's recorded `chain_target` when `real_dll` names
  EDVR itself.

**What the next hangar flight should show** (same spot as 053745, DLSS or
DLAA; read the build line first): the flat CPU line says "clocked on K of N
frames, one in 16" and prices the clocks near 0.1 ms a frame (was 1.7);
`camera rows` near zero (fresh lookups only: the writes and rebinds, about a
hundred a frame, not about 5,000); `engine motion wrapper D3D calls` a frame
far below 13,549 (the new `engine motion: flat draw bracket` line says the
runs, the draws that found the state still bound, why the game's state went
back and calls per substituted draw); the `query shortcuts` token reads
about a thousand answers a frame for the coverage depth view, a handful
checked per 64 frames and 0 wrong, and no `flat query shortcut:` line (a
state that asks again is a finding: a setter path nothing hooks). Everything
else unchanged: present p50, treated counts and streaks, the contract reasons,
motion. With ReShade in the chain the panel shows the note and the log says
`flat wrapper note: shown`; with EDHM alone, neither.

**2026-09-30 note: flight 090706, the cuts flown (Epic, `c2a06a98` matched,
on foot in a hangar, 60 s AA off then DLSS).** AA off presented 329-336 fps
(about 3.0 ms); DLSS present p50 5.99 ms (p95 8.41), GPU frame p50 3.96 ms, so
the frame is CPU-bound. The scene is heavier than flight 053745's (1,592
substituted draws a frame against 1,130; contract reductions 8,574 against
4,969), so compare per draw.

- Draw wrapper: 2,082 D3D calls a frame over 1,592 substituted draws (about
  1.3 a draw, was 12), 0.21 ms a clocked frame (was 0.89).
- Camera rows: 0.085 ms over 1,049 fresh lookups a frame (was 0.44 over 5,065).
- Query shortcuts: about 3,900 answers a frame from tracked state (depth
  view 1,974, render targets 1,590, blend 149, MRT6 151, shader 4), checked
  48-52 times each per window, 0 wrong; no state fell back to asking.
- The census clocks the render thread 1 frame in 16. Its per-clocked-frame
  total (3.74 ms) still carries that frame's clock overhead (about 73,000
  scopes), so the render thread's own figures read high.
- Frame cost of DLSS against AA off: about +3.0 ms (was +4.1 in 053745's
  exclusive legs, with the unsampled census's 1.7 ms inside).
- Largest remaining: hook entry ("other") 1.40 ms over 17,180 calls, contract
  reduction 0.42, engine motion hooks 0.37, copy checks 0.34, trace ring
  0.23, coverage 0.22, projection readiness 0.18; on job threads, the clear
  and merge observers 1.9 and 1.06 thread-ms a clocked frame.

## 81. Resolve before bloom, DoF and tone: the HDR route (design, 2026-09-30)

Design and analysis only: no code, no build, no flight. Sean asked whether the
AA pass can run on the scene's HDR image, before the game's bloom, depth of
field, tone map and grades, so they act on an anti-aliased image and treatment
stops depending on the post chain's shape (section 79's treadmill). Four F10
traces say yes when the render size R is at least the output size D: resolve
the game's HDR target once a frame at a stable moment and write the result
back into it. Upscaling (R < D) keeps today's copy-stage route.

Environment: Epic, game build 332841, EDVR v0.18.0-rc.4-33-gc2a06a98, EDHM
chained, RTX 5090, DLSS model k preset 11, 3840x2160 render and output
(supersampling 1.0), game AA off. NOT measured: FSR, EDVR TAA, game AA on,
bloom without DoF or the reverse.

**Captures** (`edvr_logs\traces`; the scenes repeat, reversed). off = AA, bloom
and DoF 0 in Elite's file; on = Bloom 3, DoF 2. A trace keeps 3 frames; draws
record SRVs only for known tone and copy passes, so other inputs are inferred
(inf) from sizes, formats and bytecode; recorded ones are rec. Hashes: 8 hex.

| session | star (cockpit, bright star) | 3D hangar |
|---|---|---|
| 091933, on | 85998: frames 85543, 85679, 85814 | 87229: 86970 (+2 menu frames, no scene) |
| 092433, off | 45737: 45734-45736, consecutive | 39788: 39785-39787, consecutive |

**Why 091933 refused.** 9,640 frames were treated, the last in the window
ending 09:21:18, then none. Refused (5 s reason lines): no-known-tone-pass
1,616 frames in 9 windows; no-observed-hdr-writes 25, engine-source-not-ready
6 and incomplete-jitter-frame 6 (one or two frames a window: transient).
1,156 tone frames follow Sean's change to Bloom 3 and DoF 2 (file read
09:21:21): the tone pair became F9CFC798F21E9AEA/E8948389387DA083, not
whitelisted. Stand-down entered after 449 frames (F8 named Bloom and DoF); the
F10 at 09:23:19 woke it and 441 more were refused. The other 460 fell in
three windows ending 09:20:38-09:20:58 with the file at 0, 0, 0. Session
092433 (all 0) refused 647 frames the same way in three in-game windows (one
a 56 s stand-down; F8 said "send your logs"): a clean file does not mean a
recognised chain. In 091933 the tone pass took five pairs:
F9CFC798/FEE777E9 (game off: known), F9CFC798/E8948389 (game on: no),
CFA91824/9270C355 (a menu frame: known), 43CA9F1C/E8948389 (a menu frame: PS
no), F9CFC798/6E83D02E (3D main menu: no).

**1. The scene image H.** One texture per trace: R11G11B10_FLOAT (f26),
3840x2160 = D, with depth R32G8X24_TYPELESS at the same size, the same pair in
every frame (85543 and 85814 are 271 frames apart). Each frame opens with one
resource-write event before H's first draw; two compute dispatches write H
through a UAV early. Draws into H: 27-28 hangar, 53 star off, 63-64 star on
(22-23, 44, 51 with the scene camera). The last draw into H is camera-bearing
in 10 of 10 frames but its pair differs by scene (81216C77/A2965EC2 star;
94D5C556/912477AE hangar, a sprite of 29-34 instances): no hash names it.

**2. The anchor.** The game's exposure reduction (CFA91824/0E0C65DA at 960x540,
then 320x180, then 1344A274 twice) reads H at quarter size (inf) BEFORE the
last draws into H: 33-44 late draws in the star, 1 in the hangar. Resolving
"just before the first pass that reads H" is therefore too early. Stable in
all ten frames: after the last H write comes the first full-scale consumer,
and nothing writes H after it. Rule: the first draw after the first H write
with (i) no depth bound, (ii) target not H, (iii) H bound as an SRV at t0..t3
(the binding shadow's slots; the consumers seen read it at t0 or t1), (iv)
target at least half of H per axis ((iv) separates the quarter-size exposure
pass from bloom's first level, exactly half).

| frames | H draws (events) | trigger event | trigger pair, target |
|---|---|---|---|
| star on 85543 | 1067..1336 | 1389 | DFED8E1C/143AAE05 1920x1080 (15 small draws between) |
| star on 85679, 85814 | 1019..1256, ..1236 | +2 | same |
| hangar on 86970 | 1123..1213 | 1215 | 20F383BB/FDB74215 3840x2160 f9 (inf) |
| hangar off 39785-39787 | 1130..1223, 1242..1335, 1147..1240 | +2 | DEF19B03/DED87960 3840x2160 f26, t0 = H (rec) |
| star off 45734-45736 | 742..939, 1181..1364, 900..1097 | +18, +17, +18 | F9CFC798/FEE777E9 3840x2160 f27, t1 = H (rec) |

"+N" = trigger event minus the last H draw's event (2: one constant-buffer
write between). The 13 corpus traces (39 frames: menu hangar, flight, on
foot; R from 0.5 D to 1.5 D; tone FEE777E9 and EAA5F18F) give a trigger in
every frame and no H draw after it in 39 of 39, by the rule without (iii); 8
of the 13 are R < D.

**3. The chain from the scene's end** (`+B` bloom adds, `+D` DoF adds; targets
3840x2160 unless noted; f9 R16G16B16A16_TYPELESS, f26 R11G11B10_FLOAT, f27
R8G8B8A8_TYPELESS, f28 R8G8B8A8_UNORM, f53 R16_TYPELESS, f60 R8_TYPELESS).

| pass (VS/PS) | target | star off | hangar off | star on | hangar on |
|---|---|---|---|---|---|
| late draws into H (glass, particles, glows; section 4) | H | 33 | 1 | 44 | 1 |
| game copy of H, DEF19B03/DED87960 (t0 = H rec) | f26 | - | yes | - | - |
| +D 20F383BB/FDB74215 (t0 H, t1 depth-like: inf), /80631A09 (2 outputs), /E16E27A0 | f9 x3 | - | - | - | yes |
| +D 20F383BB/BF2302BC (lerp H toward blur t1: inf) | f26 | - | - | - | yes |
| +B DFED8E1C/143AAE05, then /5ECFE708 x4 down | 1920x1080 .. 120x67 f26 | - | - | yes | yes |
| +B 129F602B/8826CACC x4 up | 240x135 .. 1920x1080 f26 | - | - | yes | yes |
| DEF19B03/831DF02E x3 + 7 dispatches | 480x270 f26, f53, f60 | yes | yes | yes | yes |
| tone F9CFC798/FEE777E9 (t0 a 3D texture, t1 H or its copy; rec) | f27 | yes | yes | - | - |
| tone F9CFC798/E8948389 (t1 H, hangar: DoF result; t2 bloom; inf) | f27 | - | - | yes | yes |
| output copy 20F383BB/DED87960 (t0 = tone target, rec) | f28 output | yes | yes | yes | yes |
| panel A888D510/015EF934 (hangar 8-9 UI draws), final 1D65FB79/9F366C4E | output | yes | yes | yes | yes |

Bloom adds 9 passes and the E8948389 tone; DoF adds 4. In the hangar the DoF
passes replace the game's plain copy of H, and their composite writes a NEW
f26 texture that bloom and tone then read (inf); DoF 2 drew no DoF pass in
the star. User 2's pair 20F383BB/5AA08A96 then /2375CCCC (section 79) is in
none of the four traces or two logs. Bytecode: 5AA08A96 is an alpha-1 copy,
2375CCCC 193 instructions on one texture (12 taps, an eighth threshold, a
0.75 constant): FXAA-shaped, inferred. Both are in Sean's own 3D-menu chains
of 09-24 and 09-27 (frame 9624 of 20260927_045812: tone 6E83D02E, both, the
copy). They look like game AA, not bloom.

**4. After the anchor.** No draw writes H. The saved post-pass bytecode
declares no scene constants (CB1): DoF CB2[3..4], tone CB2[16], 831DF02E
CB2[7], the copy none; bloom draws carry no camera. The scene camera changes
every treated frame (the jitter: b092, b775, 9ff2 star; 6b9c, fa3c, de8d
hangar) and is shared by every H draw. The UI draws (1 in the star, 8-9 in the
hangar) use one camera that never changes (hangar 05ec in BOTH sessions,
jitter on and off): not jittered. The late draws use the jittered camera (CB1).
By vertex signature: packed-vertex meshes (2CECEC30 x15; 81216C77 x11, the
cockpit holo family, `kHolo` in eye_draw_snapshot.h, with a 16 KB lit pixel
shader), particle emitters (1B285CBC: atlas and align-blend inputs), a
lighting-volume mesh (5559BD94) and, in the hangar, one instanced glow sprite
(94D5C556). They lie inside the resolved image, so nothing drawn after the
anchor needs an un-jittered camera.

**5. Copy-back.** H is R11G11B10F at R = D, the same resource every frame. No
game history pass is visible (the trace records no reads; v4 settles it);
after the anchor H is read by the game's copy (hangar), DoF, bloom's first
level and the tone, this frame only. The exposure reduction reads the
pre-resolve H (quarter size, harmless). A backend writes fp16, so
CopyResource cannot go back. The resolver already keeps a private copy of its
input (`g.color`) and a finish that picks backend or raw input per pixel by
the rejection mask (flat_mono_resolve.cpp): retarget the finish to H, as a
pixel-shader draw into an RTV over H (always available) or the existing
compute through a UAV (H has UAV binding here; check the flags). R11G11B10F
requantises to the game's own precision; history stays fp16 in the backend.

**Design.**

- Gate: HDR route iff `experimental.temporal_aa_before_post` is auto (off
  until flown, then the default), a temporal mode is selected, R >= D, the
  route's E = R (every mode at R = D; DLSS, DLAA, FSR above; EDVR TAA
  evaluates at D above D so it stays on the copy route) and H is R11G11B10F,
  as the selector requires today (another format declines with a named
  reason). Else the copy route as today; so does a frame with no H (2D
  menus, loading).
- Recognition: a sibling of `flatSelectMonoFrame` over the records so far (H,
  one depth, one camera hash, sources) WITHOUT the tone and copy requirements,
  run at the trigger. No trigger by frame end: chain-shape refusal
  `no-hdr-consumer`.
- At the trigger, in the draw scope and the resolver's isolation: CopyResource
  H to `g.color`; `prep`; backend at E = R; finish into H; restore; the game's
  draw then reads the resolved H. No SRV substitution; the copy stage does not
  resolve again; verdict `treated-jittered-hdr` tells the routes apart.
- DLSS (dlaa.cpp:427): IsHDR (1<<0) | MVLowRes (1<<1) | DepthInverted (1<<3) |
  AutoExposure (1<<6), today MVLowRes | DepthInverted. No MVJittered (vectors
  are unjittered); InPreExposure = InExposureScale = 1; output R16G16B16A16F,
  `g.color` in H's format (the resource cache key gains the route). The
  feature key gains the hdr bit (flags are creation-time); a route flip resets
  history. The game's true exposure sits in the tone pass's constants, tied to
  the variant: AutoExposure first.
- FSR 3.1 (fsr3_engine.cpp:478): today ENABLE_DEPTH_INVERTED (+INFINITE,
  DEBUG_CHECKING); add ENABLE_HIGH_DYNAMIC_RANGE and ENABLE_AUTO_EXPOSURE, keep
  exposure null and preExposure 1. Names are from memory: the 3.1 header is
  fetched at build, not in the tree, so verify there.
- EDVR TAA (`taa`, flat_mono_shader_source.h): blend and 3x3 clamp on
  c' = c / (1 + max3(c)), inverse c' / max(1 - max3(c'), 1/65504); history
  linear fp16. Its blend of tone-mapped bytes would flicker on HDR highlights.
- Un-jitter: not needed for these chains (section 4). Contingency, unbuilt:
  count draws into H after the trigger (`late-hdr-writes`), log the pair once,
  latch the route off for the session after 3 such frames. If a chain needs
  it: undo the injector's row edit on the scene constants at the trigger
  (negated phase); that touches the camera path.
- Whitelist: not consulted by the HDR route and frozen (no new hashes for
  R >= D); kept for the copy route, the menu path and the copy's own checks.
  `no-known-tone-pass` and `no-known-output-copy` do not arise on its frames.
  Stand-down: unchanged, its structural set gains `no-hdr-consumer`, probe
  frames run trigger and selection without the resolve. F8: chain-shape words
  only for a `no-hdr-consumer` run; drop the Bloom and DoF advice while the
  HDR route is active; keep the Anti-aliasing advice.
- Game AA: not solved. FXAA after the tone runs on the resolved image (softer,
  double AA, no refusal); game TAA's jitter conflicts with EDVR's; keep the
  advice; MSAA is refused by the resolver's texture checks as today.
- UI and HUD: unchanged; holograms and sprites are inside H in both routes,
  panel and final follow the copy on the UI camera. RCAS
  (`fix.render_sharpness`) stays at the copy on the game's LDR image, which
  keeps the copy's recognition (in all ten frames): `flatSharpenView` must
  take the game's own source.

**Risks.** (1) DLSS HDR with AutoExposure on a sun disc, a bright star, hangar
lights: pumping or ghosting; flight-decided. (2) DoF's depth is jittered
against a resolved colour (under half a pixel): halos? (3) The half-size test
is a heuristic; `late-hdr-writes` and the F10 trace check it. (4) Cost:
private copy 33 MB, fp16 output 66 MB at 4K; expect 0.1-0.3 ms over the copy
route (same prep, backend, finish); read `GPU resolve`. (5) R < D (upscaling;
8 of the 13 corpus cells) gains nothing, and the look changes for users on
known chains (bloom stops sparkling, DoF and tone see the AA'd image).

**Rigs and fixtures.**
- Add the four traces to `tools\flat_temporal_test\traces`, trimmed to the
  frames the rigs pin (7.1 MB untrimmed; the set is 29 MB), pin per frame H,
  its first and last event, the trigger event and pair (table above), 0 H
  writes after it, and run the rule over every fixture: a trigger in each
  frame the tone route selects, none in 2D menus.
- A `--trace-chain <file>` mode for flat_temporal_test: the pass-level dump
  behind this section (a scratch program today), so the pins are re-derivable.
- Mutations: a late H draw after the trigger (counter, latch); a half-size
  non-consumer between H writes (only (iii) skips it: show it fires early
  without); H changing identity mid-frame; two H candidates.
- Trace v4 (EDVRFTR4; FTR3 still read, new fields zero): SRV t0..t3 for
  every draw with no depth and a target of at least a quarter of H's area and
  for every dispatch (a compute consumer of H before the trigger would read
  the jittered image unseen; none here); H's bind flags; a Resolve marker.
  Old traces lack (iii): the rig applies (i), (ii), (iv), which can fire
  early but never late.
- Resolver rig: an HDR input (0..1000, R11G11B10F), TAA backend, a jitter
  walk: converges to the unjittered reference, no firefly ringing,
  requantisation under half an ulp. Backend flags as pure functions; LDR
  flags stay bit-identical. Replay each trace through the stand-down and F8
  rigs: no stand-down, no warning for the four chains.

**Flight plan.** (1) Observe only: trigger and selection run and log `flat hdr
route:` per verdict plus a 5 s census (triggers, declines by reason,
late-hdr-writes) while the copy route still treats; four legs (star, hangar,
each off and on), SS 1.0, EDHM chained, an F10 in each for a v4 trace. Pass:
one trigger per 3D frame, 0 late writes, the pairs above. (2) HDR route on,
DLSS, the same legs against AA off and the copy route on the off legs. Pass:
on legs treated with 0 no-known-tone-pass, cost within 0.3 ms of the copy
route; judge sun and light edges, bloom sparkle, DoF edges. (3) FSR, EDVR TAA,
game FXAA on, ReShade chained; then the default flips to auto.

**Decisions (Sean, 2026-09-30).**
- (a) The HDR route ships behind a flight key,
  `experimental.temporal_aa_before_post = off|auto`, default OFF until flown;
  then the default becomes auto (on where R >= D and H is R11G11B10F).
- (b) DLSS starts with AutoExposure; the game's own exposure is taken later
  only if highlights look wrong.
- (c) Upscaling (R < D) keeps the copy route and its whitelist for now.
- (d) With the game's AA on, treat the frame anyway (softer) and keep the F8
  advice to turn the game's AA off.
- (e) The trace fixtures go in the repo, trimmed to the frames the rigs pin.

- ruled out: user 2's 20F383BB/5AA08A96 and /2375CCCC as bloom's passes,
  because they are an alpha-1 copy and an FXAA-shaped filter, absent from
  Sean's in-game bloom chains and present in his 3D-menu chains of 09-24 and
  09-27.
- ruled out: Elite's settings file as the predictor of refusals, because 460
  frames in 091933 and 647 in 092433 were refused with all three at 0.
- ruled out: "just before the first pass that reads H", because the exposure
  reduction reads H at quarter size before 1 to 44 late draws in 10 of 10.
- ruled out: the last camera draw's hash as the anchor, because it differs
  between star and hangar.
- ruled out: un-jittering as a phase-1 need, because no draw after the anchor
  uses the scene constants and the UI camera never changes.
- ruled out: CopyResource for the copy-back, because the backend writes fp16.
- ruled out: DoF as the cause of the star's refusals, because DoF 2 drew no
  DoF pass there; the bloom variant's tone pixel shader alone refused it.

**Implementation note (2026-09-30).** Built on branch `claude/flat-hdr-route`
from main 1de97469, decisions (a) to (e) all in. It was built with the key off,
so the tree treated every frame as before until someone set it; it then flew
the same day and the key is auto by default (the bullets from Flight on, near
the end, say how). The bullets down to the flight plan describe the build as
it stood before that flight: what the rigs pin, not what a headset showed.

- Pieces. `src\d3d11\flat_hdr_route.h` is the pure half (key, detector,
  selector, R >= D gate, late-write latch, census window, every log line).
  `hdr_backend_flags.h` holds the DLSS and FSR creation flags as pure
  functions, static-asserted against both SDKs' headers. `flat_runtime.cpp`
  wires them: the detector in the draw scope, `hdrSelectAtTrigger`,
  `treatHdr`, `hdrReadKey`, `hdrFrameEnd`. `flat_mono_resolve.cpp` and three
  new shader variants are the resolver's HDR path; `dlaa.cpp` and
  `fsr3_engine.cpp` take `hdr` as part of the feature or context key;
  `flat_trace.h` is EDVRFTR4.
- The key. `experimental.temporal_aa_before_post = off|auto`, developer tier,
  live (read at every Present). Built with default off; auto since 2026-09-30
  (the flight, below). A line in the file that is not `auto` reads as off, so
  a typo leaves the copy route and never switches the route on by accident; no
  line at all reads as the default. A change wakes the stand-down, restarts
  history and, going off, rearms the latch. It is in the ini, the config
  contract, the settings schema and the flat allow-list; it has no panel row.
- Key off. The detector still runs, observe-only, on every watched draw.
  Rules (i), (ii) and (iv) compare what the draw scope already holds; the four
  pixel-shader slots are the binding shadow's identities, resolved through the
  cached `view()` the tone and copy draws already use (no context Get, no
  driver call), and only for a draw that passes them; nothing acts on the
  answer. The
  trigger decision is a per-window token printed every 5 s with its zeros:
  frames with a trigger, frames with none, late writes, and what the selector
  said at each trigger (`selection=`). Its cost is its own family in `flat cpu
  5s:`. The selector runs at the trigger too, so the key-off flight already
  reads what the route would refuse, and why.
- Key auto. At the trigger draw, before the game's pass runs: the selector
  (no tone or copy requirement, R >= D, H R11G11B10F), then `treatHdr`. It
  repeats the copy route's handoff checks, copies H into the resolver's
  private `g.color` (H's own format), runs prep and the backend at E = R,
  finishes into H through a pixel shader into a render-target view over H and
  restores the game's state. The game's pass that reads H next sees the
  resolved image with its own bindings untouched; the copy stage does not
  resolve again. Verdict `treated-jittered-hdr` (`treated-zero-jitter-hdr` at
  zero phase). A check that fails before anything is written declines, and the
  frame is the copy route's exactly as with the key off. A backend refusal is
  recovered by the spatial pixel shader into H (jitter resampled away, no
  history) and counted as the refusal it is. RCAS stays at the copy, on the
  game's own LDR image, only where the copy route's selector recognised that
  copy.
- Not the route's frames. R < D: `hdr-route-needs-render-at-least-output`
  (transient, never a stand-down by itself), copy route and whitelist as
  before. EDVR's TAA above D: selected, then declined at the treatment
  (`route-does-not-evaluate-at-render-size`), and F8 does not claim the route.
  No H (2D menu, loading): `no-hdr`. Two H candidates a consumer could read:
  `conflicting-hdr` (transient).
- Backends. DLSS flags: IsHDR | MVLowRes | DepthInverted | AutoExposure
  (0x4B); the LDR set 0x0A is unchanged bit for bit; never MVJittered. FSR adds
  ENABLE_HIGH_DYNAMIC_RANGE (1<<0) and ENABLE_AUTO_EXPOSURE (1<<5) to its LDR
  set, exposure null, preExposure 1. The bit is in the DLSS feature key and
  the FSR context key, so flipping the key mid-session remakes each once (a
  hitch, logged) and history starts; with auto from startup the first feature
  is already the HDR one. EDVR's TAA blends and clamps in c / (1 + max3(c))
  space with fp16 history and inverts on output.
- Stand-down and F8. `no-hdr-consumer` joins the structural set. It replaces
  the copy stage's structural refusal of a frame that has an H and no consumer,
  and nothing else: a frame the copy route treats keeps its verdict. A probe
  frame runs trigger and selection without the resolve, and a probe the route
  selects ends the stand-down. F8, while the route is active (selected, and
  evaluating at R): no Bloom or DoF advice, the Anti-aliasing advice kept
  (decision (d)); the log line says ", HDR route active". As built, the route
  also merged its own refusals into a frame's stand-down verdict; that was a
  bug at R < D, found and fixed on 2026-09-30 (the last bullets say how).
- Latch. After the trigger, a draw into H, a dispatch whose UAV is H, an
  explicit write to H, or a draw into a scene-shaped HDR target that did not
  exist at the trigger counts as `late-hdr-writes`. A treated frame with any
  counts toward the latch; the third in the session (not consecutive) turns
  the route off, and the copy route treats from then on until the key goes off
  and back to auto. The un-jitter contingency stays unbuilt.
- Trace. EDVRFTR4 adds `hdrSrv[4]` to an event (flag 1<<6), read only for the
  draws the detector asked about, and a Resolve marker (event kind 5: H, the
  trigger's pair and place, the route's verdict). EDVRFTR3 still parses; the
  corpus files are not rewritten.
- Fixtures. The four captures, cut with `--trace-trim` from 12 frames (6.2 MB)
  to 7 (3.7 MB), headers and events verbatim, so each replays to the hash it
  was recorded with (`traces\manifest.txt` says which frames and why). Pinned
  per frame: H's draw count and its first and last event, the trigger's event,
  pair, target and format, no write after it, the route selects it. Event
  indexes, "+n" = trigger minus H's last event: star with Bloom 3 and DoF 2,
  85543 (63 H draws, +53) and 85814 (64, +2), DFED8E1C/143AAE05 at 1920x1080
  fmt 26; hangar, same settings, 86970 (27, +2), 20F383BB/FDB74215 at
  3840x2160 fmt 9, and 86701, a 2D menu frame with no H and no trigger; star
  off, 45734 (53, +18) and 45735 (53, +17), the tone F9CFC798/FEE777E9;
  hangar off, 39785 (28, +2), the game's copy of H, DEF19B03/DED87960. The
  corpus is 17 files and 46 frames: 45 have an H and all 45 a trigger, none
  has a write after it, each of the 42 frames the tone route selects has one,
  and the cells below the output (R < D) are refused by name.
- Rigs. `flat_temporal_test`, 112 new pins in `flat_hdr_route_tests.h`: key
  parsing, the flag sets as pure functions, the detector on synthetic streams,
  rule (iv) over nine extents, rules (i) to (iii) one at a time and each of
  t0..t3, every selector refusal, the latch, the census lines and the
  selection tally, the stand-down and F8 verdicts, the EDVRFTR4 round trip with
  EDVRFTR3 still read, the corpus, and source pins for what needs an NVIDIA
  GPU (DLSS flags, feature key, the resolver's calls). `--trace-chain <file>`
  prints what the detector finds in each frame of a trace, `--trace-trim` cut
  the fixtures. `flat_mono_resolve_test` on WARP, 37 checks in
  `flat_hdr_route_gpu_tests.h`: the backend is told HDR and handed R11G11B10F
  in and fp16 out, a reset frame is the jittered input resampled, a resolved
  frame is the backend's output where history is trusted and the input where
  not, TAA's bounded space (a 10000 firefly on 100 comes out near 111, a linear
  blend would give 1090; no NaN, no overshoot, flat fields stay flat under a
  jitter walk), a refusing backend leaves H bit for bit what it was, the
  spatial recovery, refusals before any write, the preflight, the copy route
  after an HDR frame. `fsr3_engine_test` (l), AMD's real port on WARP: the HDR
  and auto-exposure bits reach `desc.flags`, a still 40 comes back as 40, AMD's
  debug checking says nothing, a flip remakes the context each way.
- Mutations caught, each a scratch run restored afterwards. Detector: rule
  (iv) without its pixel of slack (1 failure), rule (iii) dropped (13), explicit
  writes uncounted (2), ambiguity unmarked (4), the latch at four (2), the
  extent band dropped (74), late draws uncounted (12), the selector without
  R >= D (26). Resolver: TAA blending linearly (5), TAA's inverse missing (4),
  DLSS not told HDR (6), the finish inverting its reject test (3), the spatial
  recovery forgetting the jitter (1). Wiring: DLSS flags without hdr, the
  feature key without the bit, ensureFeature and the FSR call not handed the
  bit (1 each), FSR flags without the bits (2), FSR context key without the
  bit (1). On the real star-off frame: a late H draw, a full-size impostor
  between H writes, a second H, H changing identity after the trigger, an
  explicit write and a dispatch into H, a copy of H.
- Corrections to the design, found in the code. (1) "Requantisation under half
  an ulp" holds only where a device rounds to nearest: the rig's device (WARP)
  truncates float to R11G11B10F toward zero, worst miss 0.969 ulp, every miss
  below the exact value, so the pin is one ulp. (2) fp16's 65504 is not H's
  ceiling: R11G11B10F stops at 65024 (R, G) and 64512 (B), and a value above
  stores an infinity that the game's bloom blur would spread; the finish clamps
  per channel and TAA's inverse is bounded to it. (3) Rule (iv) needs a pixel of
  slack: the half-size target of an odd extent is floor(w/2), so "at least
  half" missed its own first bloom level. (4) Trace v4 is narrower than
  written: slots only for the draws the detector asks about, in a field of
  their own, no dispatch slots and no bind flags; a compute consumer of H
  before the trigger stays invisible (none in the four captures). (5) "No
  trigger by frame end: `no-hdr-consumer`" replaces only the copy stage's own
  structural refusal; a frame the copy route treats keeps its verdict. (6) The
  four captures are 6.2 MB untrimmed, not 7.1.
- What the flight must show. Install on the Epic install (its ini kept) and
  read the log's version line first (`edvr_log.py --expect-build HEAD`). SS
  1.0 (R = D), DLSS, EDHM chained, game AA off; four legs of a minute or two:
  the hangar and a bright star, each with Bloom 3 and DoF 2 on, then both 0.
  (1) Key off, the default, all four legs, an F10 on each. Pass: a `flat hdr
  route 5s:` line per window with `key=off state=observing`; on 3D windows
  `trigger` equals `hdr-frames`, `none=0 ambiguous=0`, `late-hdr-writes=0 (in
  0 frames)` and `selection=selected:N` alone; on 2D menu windows
  `hdr-frames=0`; `last-trigger=` names each leg's pair from the list above;
  the `hdr route` family in `flat cpu 5s:` small beside `state trackers`.
  (2) In the same session set the key to auto in the ini (live), repeat the
  four legs. Pass: the "changed" line, then the `flat route: ... (HDR route:`
  line, `key=auto state=active`, `treated` equal to `trigger`, `declined=0`,
  `late-hdr-writes=0`, `last=treated-jittered-hdr`, no `no-known-tone-pass`
  refusal on the Bloom and DoF legs, no stand-down, F8 without Bloom or DoF
  advice, `GPU resolve` within 0.3 ms of the copy route's on the off legs. Look
  at: a sun or star disc and the hangar lights (AutoExposure pumping or
  ghosting), bloom sparkle, DoF edges (halos from jittered depth against a
  resolved colour), the UI, RCAS. Stop and read the log if: `late-hdr-writes`
  above 0 in a treated frame (the pair is named once, the latch trips at
  three), any `declined` (its reason is logged, a dozen a session),
  `spatial-fallback` (`hdr-spatial=` in the renderer line), `ambiguous` above
  0, `selection=` naming anything but `selected`, or `hdr-frames=0` in a 3D
  window (H is not R11G11B10F, or the candidate rule does not match this
  build). (3) Later: FSR, EDVR TAA at R = D, game FXAA on, ReShade chained;
  then the default flips to auto (it did, on the flight below, ahead of these).
- The log, verbatim. Once a session each:
  `flat hdr route: experimental.temporal_aa_before_post=<off|auto> (read at
  startup|changed) at frame=N: <what the key does>`
  `flat hdr route: first trigger at frame=N seq=Q (key=K): VS=<hash> PS=<hash>
  target=WxH fmt=F reads the scene HDR WxH at tS; its D draw(s) ran seq a..b;
  selection: <the route could resolve this frame | the refusal>`
  `flat hdr route: frame=N wrote the scene HDR after the trigger (key=K): X
  draw(s), Y dispatch(es), Z explicit write(s); the first is seq S VS=.. PS=..
  target=WxH; a treated frame with such writes counts toward the latch (3
  turns the route off)`
  `flat hdr route: turned off at frame=N after 3 treated frame(s) wrote the
  scene HDR after the resolve (the latest: ...); the copy route treats from
  here until experimental.temporal_aa_before_post is set off and auto again`
  Every 5 s while a temporal mode runs, zeros included:
  `flat hdr route 5s: key=K state=observing|active|latched frames=N
  hdr-frames=N trigger=N none=N ambiguous=N treated=N declined=N
  late-hdr-writes=N (in N frames) last=<verdict> last-trigger=VS=.. PS=..
  target=WxH hdr=WxH selection=<name:count,...|none>`
  On a decline, a dozen a session: `flat hdr route: declined at frame=N seq=Q:
  <reason> (the copy route serves this frame)`, the reason one of latched-off,
  returned-to-observation, already-treated-this-frame,
  producer-source-identity-mismatch, route-does-not-evaluate-at-render-size,
  actual-hdr-binding-or-depth-view-refused, incomplete-jitter-frame,
  engine-source-not-ready, or the resolver's own. Elsewhere: `flat route:
  <route> R=.. E=.. D=.. (HDR route: H is resolved before the game's post
  chain)`;
  `flat runtime: ... last=treated-jittered-hdr|treated-zero-jitter-hdr|
  spatial-fallback`; `flat runtime early fallback: ... output=spatial (HDR
  route, into the scene target) ...`; `dlss: the feature for eye N was created
  for the flat HDR route: HDR input and automatic exposure ...`; FSR's create
  and remake lines naming the HDR input; `flat settings warning: ... , HDR
  route active)` (and, for the supersampling paragraph, `flat settings warning:
  ... , supersampling below 1.0 (render WxH, output WxH))`); `flat cpu 5s:`
  gains `hdr route X ms (calls N)`; the renderer cumulative line gains
  `hdr-resolves=N hdr-spatial=N`; the stand-down reason `no-hdr-consumer`.
- Not built: the un-jitter contingency; the section-82 VR adapter, which
  reuses this detector (its rule (iii) already skips a copy of H, pinned,
  because the VR frame's exposure chain reads one).
- Flight (Epic, 2026-09-30). Log `edvr_gfx_20260930_125014.log`, version
  v0.18.0-rc.4-57-g1cb352d7 (`edvr_log.py --expect-build 1cb352d7` exits 0).
  The main-menu hangar and a bright star, key auto. Every `flat hdr route 5s:`
  window from 12:54:10 to 12:55:15 shows `state=active`, `treated` equal or
  nearly equal to `frames` (449 to 450 in 5 s), `declined=0`,
  `late-hdr-writes=0`, and no `turned off` line; `last-trigger=` is VS
  DFED8E1C9E191BEC PS 143AAE0597E2F7BF, `target=1920x1080 hdr=3840x2160`
  (bloom's first level, the star pair pinned above). One window (12:54:50)
  treated 389 of 417, with `selection=` including
  `conflicting-hdr-target-or-camera`: seen once, not chased. Sean: "it looks
  good, let's make it default". Not in this record: the Bloom and DoF off
  legs, FSR, EDVR TAA at R = D, game FXAA on, ReShade chained. R < D is the
  next bullet.
- Flight at R < D (Epic, 2026-09-30). Log `edvr_gfx_20260930_133152.log`,
  version v0.18.0-rc.4-65-geba67321 (`edvr_log.py --expect-build eba67321`
  exits 0). Key auto from startup (`read at startup`); Sean set Elite's
  supersampling to 0.75 with bloom on (Custom preset: AAMode 0, BloomQuality
  3, DOFEnabled 2). At 13:32:30 the first trigger, a probe frame of the
  hangar (VS 20F383BB PS FDB74215, 27 H draws, the pair pinned above), ended
  a 30.9 s stand-down that had begun at startup for no-known-output-copy and
  read no-known-tone-pass by its last probes. The window ending 13:32:43 has
  `treated=60` of 78 triggers and `selection=selected:60,
  hdr-route-needs-render-at-least-output:18` (`declined=0`: that counter is
  for a selected frame the treatment declines) as the render went to
  2880x1620 against a 3840x2160 output. After that every trigger read the
  same verdict (288 in the next window, 151 in the one after), `treated`
  stayed at 654, and the copy route refused the frames for
  `no-known-tone-pass` (299 and 149; the frames with no scene read
  `no-observed-hdr-writes`). At 13:32:49.848: `flat stand-down: entered at
  frame=36663: every frame for 5.0 s (448 frames) was refused for
  no-known-tone-pass, none treated`, and at the same instant `flat settings
  warning: shown (mode=DLAA, frames refused for no-known-tone-pass, work stood
  down, supersampling below 1.0 (render 2880x1620, output 3840x2160))` with
  the Bloom and Depth of field advice and the supersampling paragraph. The
  stand-down's probes kept reading the verdict (the window at 13:32:58 holds
  3 watched frames, 2 of them triggers), which is what keeps the published
  sizes fresh. Sean: it stands down and reports it in the menu. This is the
  first R < D frame in the record and it confirms the masking fix below in
  flight: the old merge would have read every one of those frames as
  transient. Not in this record: the return to 1.0 (the warning hiding live;
  the log ends at 13:33:00, still stood down, no resume line), the same at
  0.75 with bloom and DoF off, and a flip of the key.
- The default (2026-09-30). `experimental.temporal_aa_before_post` is auto
  when the file has no line: the `getString` fallback in `hdrReadKey`, and the
  shipped edvr.ini line and comment, say so, and `config_test` holds the two
  to one answer (it reads the shipped ini, takes the literal out of
  `flat_runtime.cpp` and proves a fallback put back to off is caught; the
  contract checker compares names, not values). A line that is present and
  not `auto` reads as off, and an absent line reads as auto: Sean's decision
  (2026-09-30), keeping the tested behaviour over reading an unrecognised
  value as the default, which would leave a mistyped `off` switched on. The
  invalid-profile case is not a default pin: `Config::getString` answers a
  fixed "off" for any key a profile refuses, whatever fallback the caller
  passes, so an invalid descriptor still cannot turn the route on (pinned).
  Existing installs: an ini with no line gets auto; a live ini seeded by a
  build since 0794b3ed carries `= off` and keeps it until edited.
- R < D under the default: a masking bug, found and fixed. The route merged
  its own verdict into a frame's stand-down verdict like any other answer.
  The order is None < Structural < Transient < Treatable and the trigger
  comes before the output copy, so at R < D every frame showed Transient
  (`hdr-route-needs-render-at-least-output`) first and the copy stage's
  Structural (`no-known-tone-pass`, say) could not outrank it: the structural
  run never reached five seconds, so a chain the copy route refuses never
  stood down and the F8 warning never came up. The same held for EDVR's TAA
  above D, which the route selects and then declines. The key off merged
  nothing and the flight was at R = D, so neither met it; the default would
  have handed it to every user below 1.0. Now the route adds Treatable only
  where it will resolve the frame (`flatHdrTriggerSeen`: selected, and the
  mode evaluates at the render size) and nothing otherwise. Pinned on the
  merged verdict through the stand-down simulation (a refused chain at R < D
  stands down and warns exactly as with the key off; the old merge is kept as
  a control that never does), and by a source pin that the route's refusal is
  no longer merged. A flip (off to auto, auto to off) runs `hdrReadKey` as
  in the flight build, changed only in its fallback and in clearing the
  published sizes (below). Confirmed in flight at 0.75 (Flight at R < D,
  above): the stand-down and the warning came up after 5 s.
- F8 supersampling line (2026-09-30, Sean). A third paragraph of the flat
  warning: "Supersampling is below 1.0. At 1.0 or above, EDVR anti-aliases
  before bloom and depth of field, so they no longer block it. Raising it
  costs GPU time." It appears when frames are refused (the warning's own
  condition: stood down for a chain-shape reason that found an output copy),
  the key is auto, the route is not treating them, and the game renders
  below the output on both axes. The sizes are the route's own measurements,
  not Elite's settings file: H's extent at the trigger (the extent refusal
  now carries it) and the swap chain's size, published as one word and read
  by the panel (`flatRuntimeHdrRouteBelowOutput`); the key off, a key change,
  a selection at R >= D and a resize clear it. The bloom, depth-of-field and
  game-AA advice stays above it. The warning's cache key carries the flag, so
  the text comes and goes live as supersampling crosses 1.0, and the log line
  carries the paragraph and, in its brackets, `supersampling below 1.0
  (render WxH, output WxH)`. The warning is now up to 12 lines (10 at the
  rig's ruler, worst case): the flat page is three rows, so a blank line and
  12 fill the card's 16 lines, which `menu.cpp` static_asserts; the card
  scales to the screen's height, so the longer warning shrinks its text a
  little. Pinned in `flat_elite_settings_tests.h`: present for refused + R < D
  + key auto and nowhere else (the key off, R = D and above, the route
  treating, frames not refused), the words, the key, the wrap at the panel's
  width and the log line; the wiring by source pins. Flown at 0.75 (Flight at
  R < D, above): shown at the stand-down with the measured sizes.
- Open, cosmetic, not fixed (seen in the R < D flight's log, as of
  eba67321). (1) The log line joins the warning's paragraphs with one space
  and no separator, and the list paragraph (Custom preset) has no final
  period, so it runs into the next: "...Bloom, Depth of field Supersampling
  is below 1.0. At 1.0...". The panel shows them as separate lines. The join
  is `flatFormatSettingsWarningLog`'s `"%s%s", i ? " " : ""` at
  `flat_elite_settings.h:451`; the missing period is `second = "Turn off in
  Elite's graphics options: " + list;` at `flat_elite_settings.h:397`. The
  next flat code change adds a separator (changing the on-screen words would
  break their exact-text pins) and updates the log-line pin that encodes the
  space, `flat_elite_settings_tests.h:359`. (2) The same line says
  `mode=DLAA` at supersampling 0.75: the flat mode label is DLAA when the last
  qualified frame was at R >= D (`nvidiaLabel`, `menu.cpp:888`, from
  `flatRuntimeNativeScale`, which only a frame that reaches a treatment, or a
  resize, updates), and every frame since was refused, so the warning says
  "DLAA is not active" for a session that is upscaling.

- ruled out: "under half an ulp" as the requantisation bound, because the
  rig's device truncates toward zero (0.969 ulp worst, 659 of 768 texel
  channels below the exact value, none above).
- ruled out: fp16's 65504 as H's ceiling, because R11G11B10F stops at 65024
  (R, G) and 64512 (B) and stores an infinity above it.
- ruled out: "at least half of H per axis" taken exactly, because an odd extent
  halves down and the first bloom level would miss the rule.
- ruled out: the contract observation's own srv slots as the detector's
  source, because they are filled for tone and copy draws only, and filling
  them for every draw changes which draws coalesce into a record.
- ruled out: a constant field as a test of FSR's HDR flag, because the port
  returns it alike under both sets (39.97 and 40.00 for 40), so the rig pins
  the flags the context carries instead.
- ruled out: the route's refusals as part of a frame's stand-down verdict,
  because the order None < Structural < Transient < Treatable lets a
  transient refusal mask the copy stage's structural one: at R < D every
  trigger says hdr-route-needs-render-at-least-output, so no refused chain
  could stand down or warn, and the F8 supersampling line could never have
  shown.
- ruled out: Elite's settings file as the source of the supersampling line's
  R and D, because the route measures both (H's extent at its trigger, the
  swap chain's size) and the file says only what was asked for.

## 82. VR on foot: resolve the world once instead of two eye passes (analysis, 2026-09-30)

Sean asked whether the flat single-image route would be more performant than
today's on-foot VR path. Analysis only: no code, no build, no flight. The
proposal: resolve the world texture once, with the flat resolver, before the
screen composite reads it; jitter the world camera through the flat camera
path and stop jittering the eyes on foot; put the resolved screen into the UI
layer as a crisp opaque quad with mips; switch at the world-screen gate.
Evidence: `edvr_gfx_20260930_100723.log` (Frontier, build c96b91f1 matched,
Pimax OpenXR, 90 Hz, game eye 2620x2533, output 4032x3898) and the code.

**The census tail is cut.** Both NumLock censuses (10:12:16.744, 10:12:34.259;
`census_offscreen` on, `census_frames` 2) hit the 16,384-line cap inside frame
0: all 16,384 lines are frame 0's (16,325 draws, 43 clears, 2 copies, 14
dispatches); frame 1 and the two eye draws are counted (`draws=4`), not logged.
Frame 0 alone held 21,919 offscreen draws, 151 copies, 106 dispatches, 133
clears. The logged 73% is 22 GUI-family draws (9307x8014, 8111x4871), 4,577
depth-only draws into a 512x512 depth and 11,726 into a 6144x4096 R16 shadow
atlas (six 2048x2048 tiles); it ends at q=16383 inside a tile. The world's own
draws (4.9k a frame into the 5040x2835 depth), its post chain, the copy into
the screen texture and the eye composite all follow and are absent. Question
1 is unanswered by this capture; the rest rests on the log and the code.

Retake, no code. The cap is per census and already at its ceiling
(`kMaxLinesCeiling` 16384, draw_census.cpp), so `census_frames=1` alone does
not help, and the `census_skip*` keys do not either (they swallow game draws
after the census has recorded them). (a) Cut the shadow load first: Elite's
Shadow Quality (fxcfg `DirectionalShadowQuality` and `SpotShadowQuality`, both
4 now) to 0 or 1, or stand where little casts, then `census_offscreen=1`,
`census_frames=1`. The tail is whole when the `DC end` line reads `lines=`
under 16384 (draws + copies + dispatches + clears under that). (b) The cheap
half: `census_offscreen=0`, `census_frames=3`, default lines: the two eye draws
with bindings, 151 copies and 106 dispatches a frame, enough for the screen
texture's size and format and any CopySubresource into it, not the tone or
copy draws. A five-line census change (skip draws with no colour target when a
key is set) is the fallback.

**Established without it.**
- The world is drawn once, flat, into the 2D screen's target and shown by one
  composite draw an eye (ui_layer_math.h, flight 09-23; `draws=2` a frame).
  Here it is 5040x2835: `fix.vscreen_res_width` auto is 125% of the 4032 eye
  width, EDVR patching the game's own 1920x1080 in memory (6 sites). Depth
  R32G8X24_TYPELESS at that size (520-4,911 draws a frame; a second one of the
  same size takes 4, likely the HUD panel's), MRT6 slot target R32G32 (114 MB).
- The composite: VS 5C36AF05 (position x SIZE, cb0 world, cb1[270..273] clip),
  PS CFE84157 (bytecode, 360 B): one sample of t0, times cb1[90].y, alpha 1.
  t0 is the 5040x2835 screen texture (vscreen tells the composite by SRV0's
  panel size). Opaque, one texture, no depth.
- On foot the gate holds (2529 of 2529 frames) and the layer takes nothing (0
  redirected draws): the screen stays in the game's frame, and DLSS (quality,
  preset K, 2620x2533 to 4032x3898) treats both eyes, whose frusta EDVR
  shifts every frame (`native_temporal.cpp`; 26,735 jitter frames).

**Answers.**
1. Chain shape: not observed. Likely the flat renderer's (H f26 and depth f19,
   then tone, copy, HUD panel with its own depth) ending in the screen texture
   instead of the back buffer; unconfirmed: the tone pair, whether the copy is
   a draw, the texture's format. The selector pins the copy to the swap
   chain's back buffer, so the copy route cannot recognise it as written; the
   HDR route (section 81) needs no output. The eye draw: above.
2. Camera: the game's first-person flat camera, not head-tracked; each frame
   named as b1 of the first non-weapon pool draw into the screen-sized depth
   that took the most pool draws last frame (`screenMotionSource`; 2530 of
   2530 namings by that depth). The weapon has its own (near 0.0675 against
   0.025, section 57). EDVR does not jitter it: the only VR jitter is the eye
   frustum shift above; the flat injector and the legacy projection patch are
   flat-profile only (`flatCameraPathWanted(runtimeFlatProfile(), ...)`);
   engine motion reads the world's rows unshifted. Eye jitter therefore only
   resamples a finished texture: the world image never gains sub-pixel
   information, and edges aliased in it stay so when static (expected; a
   flight judges it). The C3 injector could jitter it: same producer (the
   view-constant refresh FUN_1405921f0; its composer runs about 30 times a
   frame). But it is flat-gated, its ownership machine knows one main group,
   and the eye views presumably pass the same refresh, taking the world's
   phase on top of EDVR's shift unless excluded by camera identity (open in
   the camera doc). Engine motion's source rows would then carry the phase and
   need it subtracted, as the flat path does (`rowsJitter`).
3. HUD: into the world texture after the world (ui_layer_math.h: "the helmet
   HUD, drawn into the same texture"; log: "screen UI: original late GUI
   draws supply alpha coverage at 5040x2835"). The eyes get only the
   composite. In flat mode the HUD follows the copy on the unjittered UI
   camera; if it does here too, it stays crisp and unresolved under the HDR
   route (the retake confirms).
4. Weapon motion belongs to the world image. Per first-person draw of the
   source pass it rebuilds motion from the original animated vertices into a
   5040x2835 map, chosen by the first-person stencil; screen motion then maps
   it to eye pixels. Fold-in: the map and stencil become two inputs of the
   resolver's `prep`, replacing the eye mapping. Without it the flat weapon
   fallback applies (weapon pixels fail depth ownership and take the current
   colour: no smear, possible crawl), likely below today's weapon.

**Cost** (ms a frame, EDVR GPU census; game 5.8-8.1 ms; GPU p50 10.6-13.3 ms
against an 11.1 ms period, CPU p50 2.5-5 ms: GPU-bound).

| item | 10:11:54 (84 fps) | 10:12:24 (74 fps) | single image |
|---|---|---|---|
| upscaler, two eyes | 2.93 | 2.87 | 0 |
| motion prep, UI resolve | 0.28, 0.40 | 0.36, 0.40 | 0 |
| screen motion (18-19 commands) | 0.24 | 0.25 | 0 |
| sharpen | 0.25 | 0.24 | 0.25 at the layer |
| weapon motion (118-290 commands) | 0.57 | 0.94 | 0.57-0.94 |
| engine velocity | 0.07 | 0.07 | 0.07 |
| world resolve, 14.3 MP (prep, DLAA, finish) | - | - | 1.4-1.8 |
| screen into the layer, two eyes; mips | - | - | 0.35-0.55 |
| EDVR total | 4.78 | 5.15 | 2.6-3.6 |

Saving 1.5-2.1 ms (about 1.8), 11-18% of the frame; GPU p50 10.6 becomes about
8.5-9.1 and 13.2 about 11.1-11.7. The pool-family draws' own 0.6-0.9 ms stays.
The resolve line is a fit to two measurements: flat DLAA `GPU resolve` p50 is
0.41 ms at 1920x1080 (flight 090706) and 0.98 ms at 3840x2160 (sessions
091933 and 092433, RTX 5090, preset K), a line of 0.22 ms + 0.092 ms per MP:
1.5 ms at 14.3 MP. The eye DLSS runs at 0.093 ms per output MP: about the
same slope.
The layer and mips lines are estimates from traffic (63 MB written and 57 MB
read an eye, 76 MB of mips).

**Sketch.**
- Resolve: the HDR route (section 81; unbuilt, key off until flown). R = D
  here by construction (the game draws the world into the screen-sized
  target), so decision (c)'s R < D exclusion does not bind. Run it from the VR
  draw path as a small world adapter around `flatMonoResolve`, which is
  profile-agnostic, not the flat runtime: the draw hooks split at
  `runtimeFlatProfile()` (vscreen.cpp), so the flat draw scope and VR's engine
  velocity, weapon and layer paths exclude each other today. The inputs
  exist: the source depth and camera (`screenMotionSource`),
  `engineVelocitySourceViews` (the flat runtime's own call), H by its colour
  target and the first full-scale consumer. Output target: H itself; the
  game's chain then makes the screen texture.
- Jitter: the injector on the source group only; the `native_temporal.cpp`
  shift off while the route is active (the layer's jitter cancel follows to 0).
- Layer: at `uiLayerDecide`'s `kWorldScreen`, redirect the screen draw once
  the world route has treated N frames: the game's own shader, opaque,
  4032x3898 an eye, t0 a mipped copy of the screen texture (`GenerateMips`,
  about 0.1 ms) so a 5040-to-~3500 minification does not alias; RCAS at the
  composite. The door has to run layer-only on foot (no DLSS, motion prep or
  UI resolve); whether it has such a mode is unchecked.
- Gate: leave to the eye route on a refusal streak (eye history reset); a 3D
  map, or any frame without an H, stays on the eye route. A separate DLSS
  feature slot for the world (a creation costs 30-45 ms once, per the log).
- Lever beyond the saving: 5040 wide compensates for a world with no AA of its
  own. With a resolve it can come down: 3840 wide is 8.3 MP, 0.98 ms to
  resolve (measured) and up to 40% off the game's pixel-bound world render
  (HUD text sharpness trades; unmeasured).

**Risks and open questions.** (1) The VR path has never hosted this: CPU at
21.8k draws a frame (16.8k are shadows) needs the adapter to skip draws by
their depth target; flat measured about 2.5 ms of EDVR CPU at 1.6k substituted
draws. (2) The injector's eye exclusion, and the weapon's second camera (same
phase group, compatible offsets). (3) Weapon fold-in or the flat fallback for
the first flight? (4) The retake decides the chain, the format (sRGB mips) and
whether the copy is a draw. (5) Flapping at the gate, boarding and disembark
resets, the transition-flash fix (jumps reset the resolve's history). (6)
Sean: keep `vscreen_res_width` at 125% or study a lower width once the world
has AA? (7) A separate flight key for the VR world route, off by default?

**Decisions (Sean, 2026-09-30).** They answer (6) and (7).
- (1) `fix.vscreen_res_width` stays at 125% (auto) for now. Once the VR world
  route has flown, a lower width (3840) gets studied: the game's world render
  up to 40% cheaper, real AA replacing what the extra width compensates for.
- (2) The VR world route gets its own flight key, off by default like the HDR
  route (section 81), and is built AFTER the flat HDR route has flown; it
  reuses that machinery.

- ruled out: `census_frames=1` as the retake, because the cap counts lines
  per census and frame 0 alone has 22.3k events against 16,384.
- ruled out: the copy route as the on-foot world's resolve point, because the
  selector pins the copy to the swap chain's back buffer and this copy lands
  in the screen texture; the HDR route has no such term.
- ruled out: eye jitter as a way to anti-alias the world's own edges, because
  the world texture is finished before the eyes exist: it can only recover
  what the texture holds.

**Census retake, 2026-09-30 10:49:20 (`edvr_gfx_20260930_104653.log`).**
Frontier, v0.18.0-rc.4-26-gc96b91f1 (the first capture's build), Pimax OpenXR
on a Crystal Super, 90 Hz, HMD Quality 0.65 (eye 2620x2533, output 4032x3898,
world 5040x2835), on foot. Shadows lowered, `census_offscreen` on,
`census_frames` 1. The census is whole: `DC end` reads `lines=8281` of the
16,384 ceiling, `truncated=0`: 7,909 offscreen draws, 149 copies, 94
dispatches, 127 clears, the two eye draws. It supersedes answers 1 and 3 and
risk (4) above. Two limits of the instrument bear on what it proves (last
paragraph): it reads render-target slot 0 only and vertex constant buffer 0
only.

Chain (q = census sequence number; sizes 5040x2835 unless noted; H is the
R11G11B10F target, R1 the D32S8 depth; `R`n names a resource):

| q | pass | VS/PS | writes | reads |
|---|---|---|---|---|
| 3793-3927 | clears: R1 (reverse-Z, 0), R2 (R10G10B10A2), R3, R4 (R8G8B8A8), H | | | |
| 3794-7832 | the world: 3,805 draws into R2, 4,141 into R1; 534 (533 of BBE58E40) read R8, a copy of R2 made at q 7292 | pool families | R2, R1 | R8 |
| 7833-7845 | 8 dispatches, 3 full-screen draws (screen space, light lists); linear depth R9 (R32) came at 7293 | | R4, R10-R12 | R1, R9 |
| 7846-7856 | 11 deferred-light draws | 7E38A6AA/7CECABDE, 10 more | H | R2, R3, R4, R9 |
| 7857-7858 | two dispatches through UAVs (section 81's pair) | ch 5998146D, EB0245DE | H, R12 (R16) | R1, R9 |
| 7859-8156 | 288 draws, 33 pairs (sky, glass, particles, holograms) | | H | |
| 8157 | CopySubresourceRegion, H to R13 (a second R11G11B10F) | | R13 | H |
| 8158-8161 | exposure reduction on the COPY: 1261x709, 320x180, 320x180 twice | CFA91824/0E0C65DA, /1344A274 | small | R13 |
| 8162-8196 | 35 LATE draws: 27 refraction particles (they read R13), 8 others | A1CE8A95/C6CD9AEB; 5B0068AF/A5E23315 x4; 01A029C7/130FC0A7 x2; B553BB47/68ABCB9F; 94D5C556/912477AE | H | R13 |
| 8198-8202 | three draws at 630x354 reading H, R12, R14; two dispatches (exposure) | DEF19B03/831DF02E; ch B8E727E9, F1FB2EEF | small | H (t0) |
| 8203 | TONE, one triangle | F9CFC798/FEE777E9 | R15 (R8G8B8A8, UNORM views) | H (t1) |
| 8204 | the game copy, a draw of 4 vertices | 20F383BB/DED87960 | R16, the screen texture | R15 |
| 8205-8211 | HUD, 6 draws on its own depth R17 (depth test off) | B10B032B/DB899F4B x3, C4B4B334/0146ABCC, A888D510/015EF934 x2 | R16 | GUI surfaces 9307x8014, 8111x4871, 4742x824, 969x577, 512x512; the 320x180 exposure output |
| 8213, 8220 | eye composite, one draw an eye | 5C36AF05/CFE84157 | 2620x2533 eye images | R16 |
| 8226-8227 | the game's post pass on each eye image, into a 3840x2160 | 01C3B84C/DED87960 | 3840x2160 | eye images |
| 8231-8275 | EDVR's per-eye DLSS (inferred from sizes and the log's dlss lines): copy of the eye, three kernels an eye, copy out | ch 6EB95466, A2E04768, C22AB2BD | 4032x3898 | eye image, history |

**Answers.**
1. Chain shape (supersedes answer 1): as guessed. H f26 with depth f19, the
   exposure reduction, the tone, the game copy (a draw, into R16 rather than
   the swap chain), the HUD on its own depth, the eye composite. The screen
   texture is R8G8B8A8_TYPELESS with UNORM views: not sRGB, so the sketch's
   mip chain averages in gamma space unless the mip copy is taken as sRGB. The
   chain is the flat chain's, pair for pair (tone FEE777E9, copy DED87960,
   panel A888D510/015EF934, exposure CFA91824, 1344A274, 831DF02E). This
   frame has no bloom, no DoF and no E8948389 (none of DFED8E1C, FDB74215,
   129F602B, BF2302BC appears) and no 1D65FB79/9F366C4E final: the eye
   composite takes its place.
2. Where the single-image AA sits: at the tone, as in flat. Section 81's rule
   gives q=8203. H's first write is the clear (3927). After it, q=8198 has no
   depth, a target other than H and H at t0, but its target is 630x354, an
   eighth per axis, so (iv) drops it; the exposure draws read R13, another
   resource, and the copy at 8157 is a copy, not a draw; q=8203 passes all
   four. H's last draw is 8196, so the trigger is +7 (a clear, three draws and
   two dispatches between), after 35 late draws as in flat. The downsample and
   the exposure compute read the unresolved H, as in flat; the tone, the game
   copy, the HUD and the eyes follow the resolve. R = D = 5040x2835 by
   construction, so the gate binds at H's format only, and it is met.
3. Does anything write the world after that point (the un-jitter question)?
   H: nothing, on slot 0 (the runtime's `late-hdr-writes` counter has the same
   limit, below). The screen texture R16, which both eyes read: yes, two things,
   both the flat frame's post-tone writes: the game copy (8204) and six HUD
   draws (8205-8211). They sample GUI surfaces and, for C4B4B334/0146ABCC, the
   320x180 exposure output as its blurred-scene input; none reads H or R1. The
   depth they bind, R17, is their own, with depth test off. So a resolve into
   H leaves the HUD unresolved and crisp, as in flat (answer 3 above, now seen
   in a census). Not shown: whether the HUD's camera carries the world's
   jitter phase (below).
4. New: the exposure reduction reads a full-size COPY of H (q 8157), not H;
   section 81 inferred H from sizes. The 27 refraction particles among the
   late draws sample the same copy. The rule is unaffected (a copy is not a
   draw, R13 is not H), but a detector that counted a copy's source as a
   consumer would fire at 8157, before the 35 late writes. Mutation case for
   the HDR rig: H copied mid-frame.
5. The per-eye DLSS sits after the game's frame (8231-8275). The log's price
   line for this window (10:49:18) agrees with the cost table: upscaler 2.62
   ms a pair (table 2.87-2.93), prep 0.40, UI 0.35.

**Not in the capture.** (1) Constant buffers other than VS b0: the census
prints the vertex stage's slot 0 only. There it is one 208-byte object buffer
shared by the six HUD draws, the two composite draws and 74 world draws; the
camera the injector would shift is b1 of the pool draws (`screenMotionSource`)
and is not in the census. So whether the HUD draws read the world's camera
buffer, and would take the injector's phase, is unobserved, and the injector
stays off the VR path until it is. Cheapest: a census token for VS b1 beside
the b0 read in `recordDraw` and `drawCensusDrawDirect` (identity is enough), or
a camera-hash trace on foot. (2) Render-target slots 1-7: only slot 0 is read,
so writes through the G-buffer's other slots (R3, R4 and the R6 and R7
surfaces are cleared, sampled, never seen written) are invisible; "no write to
H after the trigger" rests on slot 0 here, and the runtime's binding shadow
keeps only render-target slot 0 too (`BindSlot::Rtv0`), so its counter sees
draws into H through slot 0, dispatch UAVs and explicit writes, not MRT slots
1-7. (3) One frame: flapping at the world-screen
gate, boarding and disembark, and which draws are the first-person weapon
(no stencil or pool column beyond `so=`) are not in it.

What changes in the sketch: the trigger is the tone at q 8203 on the flat
rule, unchanged; the layer's mip copy must treat R16 as UNORM data; a
mid-frame copy of H is normal. Nothing is built for the VR path before the
flat HDR route has flown (decision 2).

- ruled out: a write to H after the tone trigger in the VR on-foot chain,
  because the census shows none after q 8196 (slot 0 only; the runtime
  counter has the same blind spot for MRT slots 1-7).
- ruled out: the q 8157 copy of H as the trigger, because it is a copy into
  another resource and the rule takes draws that read H.

**Pre-build findings and the stop (2026-09-30, tree 0587d6f7).** The build of
this section was ordered with stop conditions. Four readers went through the
VR draw path and the world-screen gate, the door and the UI layer, the camera
injector and the eye jitter, and the weapon map and the resolver's prep. One
stop condition is met, so nothing was built: no key, no code, no flight. All
of it is code reading (files cited), not flight evidence.

The blocker: nothing keeps the world's jitter off the eyes.
- `flatCameraAdmit` (flat_camera_phase.h) injects every kind-3 camera it sees
  while its window is armed, Upstream owns the frame and the phase is
  non-zero. It has no per-camera or per-group filter; a camera's identity is
  its struct pointer, and "main group" is per-frame ownership state
  (flat_camera_ownership.h), not an admission test. No camera-to-role join
  exists (design-flat-camera-integration.md, lines 16, 51-52, 724).
- The eye composite reads b1 rows 270..273 in the composer's layout, and an
  Elite projection built from four tangents is the kind-3 encoding, so the
  eye camera is probably kind 3 and probably passes the same refresh. The
  injector has never run in the VR profile and no VR census exists.
- An admitted eye camera takes the world's phase (bound pair += jx / 5040),
  about half of it in eye pixels and never resolved, over 8 phases: the
  screen plane would shimmer. The layer's jitter cancel reads only the eye
  shift, so nothing cancels it.
- The injector is flat-wired: the install is gated by runtimeFlatProfile()
  (flat_camera_inject.cpp:578), Frame, Arm, Disarm and Close are driven from
  flat_runtime.cpp, and phase, size and the phase-applied note are three
  calls into the flat runtime. Disarm makes its caller the owner thread;
  that the refresh runs on the VR render thread is unverified.
- Ways to tell the eye cameras apart, none verified: (A) a field signature
  at the refresh: aspect +0x260 against 5040/2835, near +0x254 (world 0.025,
  weapon 0.0675), a bound pair near zero for the world against asymmetric
  for an HMD eye, viewport +0x2A0/+0x2A4; (B) a learned pointer, joined by
  content to the eye b1 rows the engine watch already holds
  (engine_velocity.cpp:1188, :2543), which needs composeSceneCb ported from
  c2_derive_model.h; (C) close the window at the trigger (the tone), valid
  only if every world-view refresh precedes every eye-view one; (D) the
  caller address, if the eye views use other callers; (E) the camera's
  tangents against the eye frusta EDVR itself advertises. A leak detector
  that already exists: flatCameraMeasureRowShift on the eye rows, which must
  read zero once the eye shift is off.
- Every option needs one VR on-foot census first. The injector's own census
  runs with zero mutation (kind 3, armed window, zero phase is "warming")
  and prints kinds, callers and pointers; the extra columns are per-camera
  aspect, near, bounds and viewport, the call's place against the tone draw
  and the refresh thread. Installing the detour in VR needs the flag at :578
  in place of the profile test, a registered phase source for the three
  flat calls, an owner-thread Disarm and a small per-frame driver.

What the readers established for everything else (a build needs all of it).
1. Hosting the resolver in the VR hooks. None of the seven draw thunks
   (vscreen.cpp: Draw 4423, DrawAuto 4457, DrawIndexed 4469, DrawInstanced
   4503, DrawIndexedInstanced 4550, the two Indirect at 4135 and 4162) and
   neither dispatch thunk (exposure_fix.cpp 716 and 735) tests
   g_flatComputeInternal, while every state hook does. The resolver's own
   Draw(3,0), its dispatches and the NGX or FSR calls inside it would re-enter
   the VR verdict path and the exposure fix, which and the dispatch probes key
   off the shadow's last compute shader (the game's exposure pass), and would
   count as late writes into H. The first edit of any build is a one-load
   early return in all nine, and the detector belongs in DrawAuto and the two
   Indirect thunks too, which skip beginPanelOverride.
2. CPU. A draw with no colour target (16.8k shadows) dies at one Rtv0 load.
   The ~5k coloured world draws need an Rtv0-generation memo of {resource,
   size, format}: bindingResolve is four uncached COM calls, made today up to
   three times per view per generation (the loader-panel gate, on by default
   and unmeasured, ui_depth, the eye memo). The detector must not build a
   FlatContractObservation per draw (~360 B, about 8 MB a frame at 21.8k
   draws); it reads eight scalars.
3. Late writes. Flat's observers for dispatch UAVs, copies, clears and maps
   into H are flat-only and the shadow tracks Rtv0 only; the latch needs VR
   equivalents in the shared hooks. Engine motion's state can still be bound
   at the trigger (flat flushes it first, flat_runtime.cpp:2389).
4. The gate. onFootGateTick returns at once unless the layer is live
   (fix.ui_quality > 0, a temporal mode, jitter as shipped), so the route
   requires the layer live. It enters after 2 frames, leaves after 90, and
   the journal flag lags by about a second.
5. Resolver inputs VR lacks: the world camera's CPU rows, now and previous
   (screen_motion keeps GPU copies only; engine_velocity keeps the current
   rows privately, no getter, no previous), a depth SRV accessor, and the
   reset and plan bookkeeping. Engine families: VR keys fewer than flat
   (familyForProfile), which is today's eye route too.
6. Layer-only door: absent, but additive. treat() gets a branch before :337
   that hands on an output-size black frame, calls uiLayerNoteTemporal, sets
   treated and leaves continuity alone; it must never return null (:382 is a
   permanent stand-down) and never S_FALSE with a shift (the host moves the
   FOV by it). native_sharpen composites first and sharpens after for these
   frames. About 400-700 lines over 6-8 files; S_OK with an output is the
   path DLSS takes today, so the host needs no change (one reader read it as
   touching the host; the door reader found no such change).
7. The redirect. A take leaves the game's eye unwritten, and the game's own
   post pass at q 8226-8227 reads each eye image into a 3840x2160: a mirror,
   inferred. A refused take would also leave a black eye with no per-eye
   fallback. So the layer re-issues the composite after the game's draw
   (the crisp tonemap pattern, pureDrawReissue), with beginInner's mapped
   viewport: +0.1 ms, the game's eye intact, the eye route always available.
   kWorldScreen needs a route fact in UiLayerDrawFacts and a per-eye,
   sequence-tagged "world taken" tag; a new UiLayerDecision value resizes
   g_win.decided, and ui_quality_test covers kWorldScreen.
8. Mips. None exists. The screen texture is R8G8B8A8_TYPELESS: GenerateMips
   through an _SRGB SRV (linear-light, energy preserving: bright thin HUD
   strokes are not dimmed), sampled through the UNORM view the game's shader
   expects. The game's s0 may have MaxLOD 0, which would waste the mips, so
   the re-issue binds a sampler copied from the game's with trilinear and
   full LOD (no raw PSSetSamplers entry; an unhooked call once a frame).
9. Screen motion must keep recognising. screenMotionRecognize runs only in
   screenMotionDraw, which the layer's redirect skips (vscreen.cpp:4656), so
   naming the source would stop after 2 frames and take the engine slot
   source and the weapon map with it. Route frames call the recognition and
   skip the per-eye re-issue (0.24 ms).
10. The weapon fold-in fits. Prep takes two borrowed SRVs, the weapon map
   (RGBA16F at the source size, previous minus current in source pixels,
   w = 1 valid) and the stencil (bit 0x10), and constants.route[2]; attached
   pixels take the map with screen_motion's tolerance, else reject, and
   absent inputs keep today's arithmetic bit for bit. Without it, weapon
   pixels would take the world's camera term at the wrong depth and FOV under
   DLAA: a screen-fixed weapon ghosting in turns. The map's phase term is
   open until the weapon camera's phase is observed. A WARP test belongs in
   flat_mono_resolve_test.
11. The GPU census rotation gives every section a turn, so new sections
   lengthen it for the old ones even with the key off; the route's sections
   must skip their turns while the route is off.

Proposed staging, for Sean's decision. Stage 1 builds everything above with
the world unjittered and the eye shift off while the route owns the frame:
the injector is not installed in VR, so there is nothing to leak, and one
flight reads the cost table (EDVR 4.8-5.2 ms against 2.6-3.6), the layer,
mips, door and weapon paths and the gate. A zero-mutation VR camera census
rides in the same build behind its own key, so that flight also answers the
exclusion. Stage 2 adds the world phase once the census names the mechanism.
The alternative is the census alone first.

Questions for Sean. (1) Stage 1 with an unjittered world, or the census
alone first? (2) Is the route allowed to require the layer live
(fix.ui_quality > 0)? It is on by default. (3) Install the refresh detour
in VR under a diagnostic key, for the census only?

- ruled out: excluding the eye cameras by camera identity with today's
  injector, because admission keys on kind 3 and nothing joins a camera
  struct to a role (flat_camera_phase.h flatCameraAdmit).
- ruled out: the layer taking the screen draw instead of re-issuing it, as
  the first build, because it blanks the game's eye (the q 8226-8227 copy)
  and leaves no per-eye fallback when the take is refused.
- ruled out: g_flatComputeInternal as sufficient cover for the resolver in the
  VR hooks, because nine thunks do not test it (see 1).

**Implementation note: the first build (2026-09-30).** Sean's decisions on
the stop above: (1) fly an UNJITTERED first build; (2) put the VR camera
census in the same build; (3) the route may require the UI layer live; and
the weapon: weapon_motion stays as it is, and the fold-in is built. So this
build is the route with the world at phase 0 and the eye shift off while the
route owns the frame, the refresh detour NOT installed for the route, and a
seam where the injector plugs in. The flight judges COST (against the table
above) and PLUMBING (layer, mips, layer-only door, gate, weapon), not image
quality, and says so. Key: `experimental.temporal_aa_on_foot_world = off|auto`,
default off. Keep `fix.ui_quality` on (the default): the route hands the eyes
the resolved screen through the UI layer and stays off without it, with one
log line. One more prerequisite came out of the build, below: `fix.panel_curvature`
must be 0 for this flight.

Environment the route depends on: EDVR's own OpenXR runtime (the layer-only
door is that runtime's; under the Oculus native SDK it never loads, so the
route has no door there and is untested), Pimax Crystal Super at 90 Hz, HMD
quality 0.65 (eye 2620x2533, door
output 4032x3898, world 5040x2835 from `fix.vscreen_res_width = auto`), DLSS
preset K, `fix.temporal_aa = dlss`, a 96-entry view cache, 4 HDR candidates,
warm-up 8 treated frames, grace 3 untreated frames. About 300 MB of extra
VRAM (the resolver's textures at 5040x2835, the mipped screen) and one 50-100
ms hitch when the route first treats (textures and the world's upscaler
feature are made then).

What was built, by piece (each read back against the code):
1. World adapter (vr_world_route.cpp, vr_world_route_math.h). The draw hooks
   call `vrWorldRouteDraw` once per game draw while the key is auto, the layer
   is live, the curvature is off and the on-foot gate holds (one bool load
   otherwise). It runs the flat HDR route's own detector (flat_hdr_route.h)
   through a glue function the rig pins, and at the tone (q 8203 on the census
   chain) resolves H with `flatMonoResolve` into H itself, on upscaler slot 2
   (the eyes own 0 and 1; dlaa.cpp and fsr3_engine.cpp grew a third slot).
   There is no prefix model in VR: the selector is eight facts
   (vrWorldSelect), and the world's depth and camera come from screen motion's
   naming through engine_velocity (`engineVelocitySourceIsNamed`,
   `engineVelocitySourceCameraRows`, new). Every refusal has a name in the 5 s
   line. Resets: a scene reset (the transition detector withholding an eye:
   skipEye calls `vrWorldRouteNoteSceneReset`), a frame gap, a change of depth,
   H or extent, and the resolver's own camera-cut test. The exposure chain
   reads a COPY of H, so rule (iii) skips it: pinned with H copied mid-frame.
   Nine thunks (the seven draws, the two dispatches) and the ClearState hook
   step aside for the route's own calls (`g_vrWorldInternal`); the VR hooks
   never did.
2. Jitter. None in this build (seam: `worldPhase()` in vr_world_route.cpp).
   The eye shift (native_temporal begin) goes off for the frame after the
   route becomes owned and stays off until it is released; the layer's jitter
   cancel follows to 0 by construction. Eye exclusion is for the census.
3. Weapon. Folded in: the weapon map and the stencil are inputs of the
   resolver's prep (FlatMonoResolveFrame::firstPersonMotion and
   firstPersonStencil, both or neither; attached pixels take the map's motion
   or reject their history, never the world's camera term). With the map
   absent the resolver runs today's arithmetic (the flat path's weapon
   handling). weapon_motion.* is untouched. The map's phase term is open, as
   section 82's finding 10 said; unflown.
4. Layer and door (ui_layer.cpp, native_temporal.cpp, native_sharpen.cpp,
   vr_world_mips.cpp). On an owned, treated frame the 2D screen composite
   reaches uiLayerDecide as any opaque no-depth eye draw and is NOT taken: the
   game's draw lands in its eye as always (its post pass copies that image
   on), and vscreen.cpp issues the draw once more into the eye's layer right
   after it (`worldScreenReissue`), from a mipped copy of the resolved screen
   and a trilinear sampler copied from the game's, before the verdict's state
   is undone. Refusals are named and counted (curved screen, depth-tested or
   blending draw, no mipped screen, no sampler, bindings changed, the layer
   refusing the issue, a fault) and leave the eye to the eye route; the route
   is told of a take only after every state is back. The mips are made once a
   frame (copy, then GenerateMips through an _SRGB view: linear-light, energy
   preserving; sampled through the UNORM view the game's shader expects). The
   door: treat() hands an eye whose draw the layer took a black frame of the
   output size and format (made once, shared by the eyes), after the layer's
   own preflight (`uiLayerWorldDoorGap`) says the composite will certainly run;
   otherwise the SAME call goes through the ordinary pass. It answers S_OK,
   never null and never S_FALSE with a shift. native_sharpen composites the
   layer FIRST for these eyes and runs RCAS over the result (today's order
   would sharpen black); so RCAS now also touches the UI on these frames.
   screen_motion's recognition keeps running on route frames (the per-eye
   motion reissues are skipped, 0.24 ms).
5. CPU. Depth-only draws (16.8k shadows) die at one binding-shadow load; a run
   of draws into one target pair costs two compares; a coloured draw costs two
   per-frame view lookups and one detector call; no allocation, lock or new
   D3D call per draw. The per-frame state is cleared at the boundary.
6. The census (vr_camera_census.cpp, flat_camera_inject.cpp observe-only mode,
   `advanced.vr_camera_census = off|on`, default off, VR profile only). With
   it on the flat injector's detour is installed in an OBSERVE-ONLY mode (it
   never writes a bound pair, a dirty flag or a row) and records, bounded
   (typically 250-300 lines a session, a hard cap of 724): a 5 s line, every distinct camera
   (kind, caller, view, field signature), the whole call sequence of the first
   three on-foot frames, and at the eye composite draw of the first four
   on-foot frames the eye's b1 rows 270..273 read back from the GPU with what
   EDVR advertised for that eye. A frame counts as on foot when the tone was
   seen AND Elite's journal says on foot (a cockpit, a hangar and a menu draw
   the same tone). `python tools\edvr_log.py --camera-census` reads it back
   and does the join (below). With the key off nothing is installed,
   allocated or logged (rig-pinned with a counting operator new).
7. Curvature. With `fix.panel_curvature` above 0 the game's screen draw is
   swallowed by the geometry substitution, so the layer would refuse it on
   every frame and an owned route would pay for the world resolve while the
   eye route still served the eyes. The route therefore owns no frame while
   `panelCurveWants()` holds (the predicate that decides the substitution): it
   counts as the layer not being live, and the one log line names the key.
   Sean's live ini has `panel_curvature = 0.3` with `panel_distance = 0.7`;
   the flight needs 0. A curve-aware re-issue (through panelCurveSubstitute)
   is the follow-up; it is not built.

THE KEY-OFF CONTRACT is pinned four ways. (a) The machine never leaves Off,
owns nothing, suppresses no eye shift and takes no screen whatever the frames
show (4096 random frames). (b) uiLayerDecide with worldRoute=false equals a
frozen copy of today's function for every combination of facts (10,092,544 of
them), and with worldRoute=true only the held screen's world-screen line
changes. (c) Every added hook line is guarded by a flag that is false with
both keys off, and the boundary returns at its first test (source pins,
mutation-checked). (d) The GPU census gives the route's three sections no
rotation turn until one has been called, so its sampling is unchanged. With the
key on, a frame the route does not own is today's eye route: the door asks
the layer's preflight first and the route's answer is per eye and sequence.

Rigs and what they prove (all in build.bat's gate; counts are from the build
that carried this note):
- tools\vr_world_route_test (pure, 99 checks): key parse, the machine
  pairwise and exhaustively, the predicates, the eight-fact selector in
  order, the glue (rules i-iv) and the TRIGGER on a synthetic chain built
  from the census retake's table (there is no trace of that chain to replay:
  q 8157 copy, 35 late draws into H, the 630x354 readers, the tone at q
  8203), the line formats, and the source pins of every hook. 31 mutants, all
  killed.
- tools\vr_world_route_gpu_test (WARP, 82 checks): the real adapter with the
  real shadow, Config and resolver (stub backends): key off, the happy path
  and ownership, the latch, each refusal, the layer not live, a curved
  screen, the gate lost, a scene reset, a frame gap, key off while owned, the
  5 s line read live, a 6000-draw run. 20 mutants: 18 killed; the two
  survivors are equivalent (a frame-gap reset the resolver also makes itself;
  the evaluates-at-render test the extent equality already implies).
- tools\vr_world_mips_test (WARP, 552 checks in 11 cases): the copy, the
  sRGB-view mips, the refusals, the sampler table; mutants.py is 95
  mutations over 11 rules, every anchor found once.
- tools\ui_layer_world_test (WARP, the REAL ui_layer.cpp linked whole, 92
  checks; also run by hand on the RTX 5090): the re-issue shows mip 1 in the
  layer while the game's eye shows mip 0, every state is the game's after a
  landed re-issue and after each refusal, the 1.25x layer, the door gaps.
  61 mutants, all killed after hardening.
- ui_quality_test (3106), native_temporal_test (491), native_sharpen_test
  (639), screen_motion_test, flat_mono_resolve_test and the two census rigs
  (vr_camera_census_test 134, the glue rig 56 on WARP with the real reader)
  carry the rest. What no rig runs: the vscreen.cpp forwardWithVerdict flow
  (source pins and a DLL compile only), the VR-profile hook install through a
  real CodeHook, and the game's real b1.

FLIGHT PLAN. Frontier, VR on foot, the environment above. Edit the live
`edvr.ini` with the Edit tool (never a regex): `experimental.temporal_aa_on_foot_world
= auto`, `advanced.vr_camera_census = on`, `fix.panel_curvature = 0` (from
0.3: the route stays off while it is above 0). Leave `fix.ui_quality = 100`,
`fix.weapon_stability = 1`, `fix.temporal_aa = dlss`, `fix.vscreen_res_width =
auto`. This flight judges cost and plumbing, NOT image quality: the world is
unjittered (DLAA on a static grid), so expect a less stable edge than the
eye route's jittered image. Fly a settlement on foot for two minutes, weapon
out and holstered, then board and disembark once, with the 3D map open for
ten seconds. For the cost comparison, fly the same spot with the key off
first (or read an earlier log): `EDVR ~ 4.8-5.2 ms`.
Read, in this order (HEAD is the installed build):
  python tools\edvr_log.py --target frontier --expect-build HEAD --version
  python tools\edvr_log.py --target frontier --expect-build HEAD --grep "vr world route|vr world mips|layer-only|LAYER-ONLY|EDVR GPU census: |LONG FRAME"
  python tools\edvr_log.py --target frontier --camera-census --expect-build HEAD
- PASS: `vr world route 5s:` shows `state=owned` after the first seconds,
  `treated` within 2% of `trigger`, `declined` near 0 and `selection=selected`,
  `eye-takes` twice `owned-frames`, `door-layer-only` equal to `eye-takes`,
  `late-hdr-writes=0`; one `OWNS the world` line, one `the layer took the
  screen` line and one `the layer re-issued` line per eye; one `vr world mips:
  mipped screen 5040x2835` line; in the `EDVR GPU census:` line the door items
  (upscaler, motion prep, UI resolve, in-frame screen motion) about 0 in owned
  windows, `world resolve` 1.4-1.8, `world mips` 0.1-0.2, `world layer`
  0.2-0.5, and `EDVR ~` 2.6-3.6 ms against 4.8-5.2 with the key off;
  application render p50 down by 1.5-2 ms; the HUD, the weapon and the eye
  image intact, the desktop mirror not black; `vr world route layer:` shows
  about 2 re-issues a frame and `lost while the route owns that eye` at 0.
- EXPECTED: one LONG FRAME of 50-100 ms when the route first treats;
  `RELEASED ... (on-foot-gate-lost)` when boarding; `the route stays off ...
  fix.panel_curvature` if the curvature was forgotten (then set it to 0).
- STOP: `selection=` naming anything but `selected` for more than a few
  frames; `OWNS`/`RELEASED` flapping while on foot and not in a transition;
  `RELEASED ... (frames-not-treated)` standing still; a `turned off at
  frame=` (latched) line or `late-hdr-writes` above 0; repeated `layer did not
  take the screen draw` lines or `eye-takes` below twice `owned-frames`;
  `native temporal: layer-only declined` more than a handful; `native sharpen:
  LAYER-ONLY ... got NO composite` (a black eye: the layer stands down and the
  route lets go); a black eye, a missing HUD, weapon ghosting in a turn; the
  census total above the key-off baseline; any `vr_world_route` fault line.
- CENSUS: the report has the 5 s lines, the camera table, the call sequences
  of the first three on-foot frames, the eye draws, and the offline join of
  the eye b1 rows to the cameras. It answers the stage-2 question with (A) the
  field signature, (B) the content join to the eye rows, (C) where in the
  frame the calls fall against the tone, (D) the caller address, (E) the
  tangents against EDVR's advertised frusta (usually "not compared": use the
  join's eye-camera signature lines and the eye-geometry leak= instead), and
  (F) the view pointer. Raw lines: `--grep "vr camera census"`. Turn the key
  off after this flight; it stalls a few ms eight times a session and nothing
  more.

NOT BUILT (and why): the world jitter and the camera injector for the route
(stage 2: it waits on the census naming the eye cameras); a curve-aware
re-issue (curvature must be 0); per-eye fallback when only one eye is taken
(that eye is served by the eye route with its shift off for the frame);
the Oculus native SDK (the route has no door there).

Decisions recorded: the layer RE-ISSUES the screen draw instead of taking it
(the game's eye stays intact for the mirror copy and the per-eye fallback);
mips are made linear-light through an sRGB view and sampled through the UNORM
view (the gamma decision, rig-proved); the sampler is copied from the game's
with trilinear and the full LOD range; RCAS runs after the composite on
layer-only eyes; the layer-only base is black whatever `fix.black_void` says;
the route requires the layer live and the curvature off, and says so once;
the gate is the layer's existing on-foot world-screen gate; the same
treated-frame machine as flat (warm 8, grace 3, late-write latch 3).

- ruled out: owning a frame with `fix.panel_curvature` above 0, because the
  geometry substitution swallows the game's screen draw, the layer refuses it
  on every frame (curved-screen), and the route would pay for the world
  resolve while the eye route still served the eyes (read from the code, not
  flown).
- ruled out: counting the layer-only door inside the predicate, because the
  temporal door and the sharpen pass both ask it for the same eye and sequence
  and the 5 s line would read twice `eye-takes` on a healthy flight (found by
  the merged rigs, pinned).

**Flight 1 of the route, analysed (2026-09-30).** Read and diagnosed; no code
changed. Log `edvr_gfx_20260930_161545.log`, v0.18.0-rc.4-104-gbde47f81
(`edvr_log.py --target frontier --expect-build bde47f81` exits 0). Sean turned
the census (16:17:21) and the route (16:17:40) on live from the in-VR menu and
had set `panel_curvature` 0 there (16:16:21). He stood on foot in a station
from 16:20:05 (screen motion names the source by its "hangar" rule: the
5040x2835 depth with the most pool-family draws last frame, 1,567), weapon not
drawn, so the fold-in is unflown in substance (one line, `flat resolve:
first-person motion inputs bound`). The in-VR menu was opened on foot twice
(16:20:56, fps overlay on; 16:21:36) with the route owned. Environment as in
the first-build note: native OpenXR, Crystal Super, HMD 0.65, DLSS preset K;
the route resolves at DLAA 5040x2835 on slot 2.

1. OWNERSHIP AND THE "FLAP". The route owned 14,220 of the 16,771 on-foot
   frames (84.8%, frames 13787-30558) in three episodes: 13,044 frames
   (16:20:06.1-16:22:41.9), 98 (16:23:00.1-16:23:01.3), 1,078 (16:23:12.1-
   16:23:24.8). The 69 five-second lines sum to frames 29,129, treated 14,235,
   declined 2,399 (2,396 depth-not-screen-motion-source, 3 engine-views-
   unavailable), eye-takes 28,422 (= 2 x owned), door-layer-only 28,422,
   late-hdr-writes 0, enters 3, releases 3. 31 whole windows were fully owned
   (treated = frames, none declined, 372-450 frames: 74-90 fps, mean 83.7).
   The 29 `observing treated=0 declined=0` windows (16:17:45-16:20:00) are the
   cockpit and hangar: gate no, hdr-frames 0, journal "aboard", 0 draws into
   the screen's depth. On-foot entry: 4 declines (naming not made yet) and 1
   engine-views-unavailable, treated from frame 13792, OWNS at 13800.
   THE RELEASES ARE SCENE CHANGES, not naming noise. No owned run declines
   except the 3 frames of grace that end it, and the other 2,392 declines all
   sit in two gaps: 16:22:41.9-16:22:59.96 (18.1 s) and 16:23:01.3-16:23:11.99
   (10.7 s). (a) The tallies partition cleanly: 16:22:45 has 91 selected, 216
   declined and 109 frames with no H at all; 16:22:50 and :55 have 450 of 450
   declined; no window mixes them frame by frame. (b) Screen motion logs
   "showed for 90 frames and nothing named its source" (16:22:43.1, 16:23:02.5)
   and "named again after 1,597 / 929 screen frames"; engine motion freed and
   re-made its 114.3 MB slot target at the same times. (c) The gate's depth
   count fell from 4,917 draws a frame to 22. (d) THE WORLD WAS PAUSED: the
   resolver's reset lines at each re-entry (delta-ms 18,125 and 10,750, the gap
   lengths) print origin-delta (0,0,0) and max-matrix-delta 0, so the camera is
   bit-identical to the last treated frame's, (-158.706497, 69.8441238,
   -394.400391): the player did not move and no second world exists. (e) The
   trigger keeps its shader pair but sits at seq 32-120 (about 1.9k in the
   world): the same frame graph drawing a few objects. (f) The eye route
   logs "auxiliary camera rows do not follow the head" within 4 ms of the
   first release. So the game showed a full-screen non-world view. The 3D map
   the plan asked for fits (the second gap is 10.7 s, the first 18.1 s a first
   look, the 98-frame world between them the map closed and reopened), but the
   log cannot tell map from menu. SEAN'S ACCOUNT (afterwards): "I opened the
   system and galaxy maps". The two paused-world gaps (18.1 s and 10.7 s) were
   those two maps, in the log's order 16:22:42, 16:23:01, then the exit at
   16:23:24 (which gap was which map is not recorded), so the releases around
   the maps are correct behaviour, now confirmed. The named source
   depth was 0x2636FFC0AA0, then 0x2636FFBD0E0 in the 98-frame episode, then
   0x2636FFC0AA0 again. The third release (16:23:24.8) is the exit: 2016x1949
   eye-sized depth targets and a loading composite into an eye appear in the
   same ms, and the session closes 3 s later. Only 12 decline lines are logged
   a session; they went to the entry (5) and the first 7 frames of the first
   gap, and the tallies carry the rest. A world-to-world jump (camera moved
   460 m at 16:20:44.240, LONG FRAME 153 ms, 469 MB created) did NOT release
   the route: the resolver's camera-cut reset absorbed it.
   NO FIX NEEDED. A frame with no naming draw has no world camera, and the
   route must not resolve it; the refusal and the release are the design
   working. One fact to carry: the trigger is not the census retake's tone. It
   is a 2520x1417 R11G11B10F consumer of H (VS DFED8E1C9E191BEC, PS
   143AAE0597E2F7BF) at seq 1661 after 132 draws into H (1518..1660), so the
   resolve lands before bloom, with 0 late writes in 14,235 treated frames. A
   write into H after that consumer would trip the 3-frame latch for the
   session: watch it in a settlement.
2. COST. No key-off on-foot baseline exists in this log (the frames before the
   keys were cockpit and hangar). The earlier key-off on-foot 30 s windows the
   same morning (c96b91f1, same rig) are the eye route: EDVR ~ 4.78, 5.15, 5.25
   (mean 5.06; door 3.77-3.91, in-frame 0.88-1.48). The four whole owned
   windows, ms a frame (EDVR ~, door, world resolve, mips, layer re-issue,
   layer composite, sharpen, screen motion, weapon motion, application render
   p50 with the game's share):
     window    EDVR  door resolve mips re-iss compos sharp scrM wpnM  app (game)
     16:20:46  2.858 .563 1.866  .068  .046   .284  .280 .125 .128  9.08 (6.22)
     16:21:16  3.703 .697 1.848  .053  .051   .433  .242 .094 .873 11.62 (7.91)
     16:21:46  3.791 .666 1.900  .053  .049   .392  .257 .094 .929 11.92 (8.13)
     16:22:16  3.648 .657 1.918  .053  .048   .382  .260 .104 .770 11.54 (7.89)
   EDVR ~ is 3.50 (2.86-3.79) against 5.06: -1.56 ms (-31%). The estimate was
   2.6-3.6; three windows sit 0.05-0.19 above its top. The door fell to 0.56-
   0.70 (upscaler, motion prep, UI resolve and hologram all "-"). Against the
   table: world resolve 1.85-1.92 (estimate 1.4-1.8, 0.05-0.12 over), mips
   0.05-0.07 (0.1-0.2, under), layer 0.33-0.48 counting the composite (0.2-0.5,
   in). Sharpen 0.24-0.28 is unchanged; screen motion fell from 0.23-0.25 to
   0.09-0.13 (the per-eye reissues are skipped). WEAPON MOTION is the largest
   item the route does not own: 0.13-0.93 ms (55-300 calls a frame) with no
   weapon drawn, 0.57-1.18 in the key-off baseline, so it is not new; it is
   21-25% of EDVR in the three heavy windows. The GPU is the limit: frame gap
   p50 0.09-0.10 ms and application render 11.5-11.9 ms in the heavy view
   (74-86 fps). The eye route would add about 1.56 ms (about 13.1 ms, 76 fps;
   an estimate, not measured). CPU: draw-hook CPU is 1.66-2.04 ms a frame
   while owned (mean 1.94) against 0.73 and 1.47-1.78 in the key-off windows,
   whose scenes were lighter (4.2k and 4.9k screen-depth draws against 4.8-
   5.1k): a rise of 0.2-1.2 ms is possible and unproven. The route's own CPU
   (detector, resolve issue, layer re-issue) needs its own clock. The
   `submits` figure of `native timing CPU` is one frame's wall time with
   waits (0.5-11 ms owned, 8-9 ms in the gaps, 0.3 ms in the cockpit): not a
   CPU cost. FIRST TREAT: the world NGX feature took 126 ms to create (`dlaa:
   ... for the VR world (upscaler slot 2) ... made in 126 ms`, 16:20:06.003).
   There is no LONG FRAME line for it: the monitor logs one per 5 s and 60 a
   session at most (perf_monitor.cpp kDropLogEveryMs, kDropLogMax); it had
   printed one at 16:20:03.8, which is the game loading the scene (155 ms of
   game work), and the 60th was at 16:22:30, so no hitch of the gaps is
   logged. The baseline to fly: one spot, standing still, no menu, the same
   scene in view, the route toggled LIVE from the in-VR menu (Experimental
   page): 90 s auto, 90 s off, 90 s auto. Read the 30 s windows that fall
   wholly inside a leg (they end at :15.8 and :45.8): EDVR ~, application
   render p50 and frames a window.
3. CAMERA CENSUS (`edvr_log.py --camera-census`): 470 lines, 4.63 million
   refresh calls over 30,345 frames, all on the render thread (owner 13304),
   hook installed from 16:17:21.665, no fault: the detour runs safely in VR.
   THE EYE CAMERAS ARE KIND 5 AND THE ON-FOOT WORLD CAMERA IS KIND 3. On foot a
   frame holds about 60-68 kind-3 calls, exactly 6.0 kind-5 calls (2 eyes x
   the 3 call sites +0x594E13, +0x594EAB, +0x594FE1), 29.8 kind-0 and 15.2
   kind-1 calls, and no kind 4. The eye b1 rows 270..273 read back at the eye
   composite draw equal the eye camera's composed rows to 1e-5 in all 6
   joinable draws (measured 0.178391, the advertised frusta's 0.178391): eye 0
   is camera 0x25FED68B4D0, eye 1 is 0x25FF2358330, both kind 5, aspect
   1.03441, fov 1.59971, near 0.025, far 50000, off-centre +0.1783 and -0.1785,
   refreshed once a frame AFTER the tone at draws 6067-6069, immediately
   before each eye's composite draw (6068, 6069). The world is ONE camera
   object, 0x25FEFC53770, refreshed 54 times a frame as kind 3 (18 per call
   site, 5 views), all BEFORE the tone (draws 4163-6049). Before the tone too:
   kind 0 x8 (far 2-30 m) and kind 1 x6 (orthographic, near = -far), shadow-
   like; a kind-0 camera (near 0.1, far 1000) follows it x6. Camera IDENTITY is
   useless: 0x25FEFC53770 was the left eye camera in the cockpit (kind 5,
   off-centre +0.1785, first seen 16:17:21) and is the world's kind-3 camera on
   foot. The eye leak baseline with the shift off and no world phase is
   |leak| <= 9.4e-9 NDC (half a pixel at 5040 wide is 2e-4). The reader labels
   a camera by its first-seen kind, so it calls 0x25FEFC53770 "other kind",
   finds no world camera and answers (C), (D) and (F) "not enough calls
   logged"; the logged sequences answer them as above. The `changed:` budget
   (24) went to the shadow cascades' drift in the first frames.
   STAGE 2 IS DESIGNABLE, and the exclusion already exists: flatCameraAdmit
   answers Unsupported for kinds 4 and 5 (never mutated) and Inject only for
   kind 3. Design: install the injector in VR while the route is Warming or
   Owned, the route as its Upstream owner and observeOnly off; keep the
   admission table as it is (kind read on every call, never cached per
   camera); close the window at the route's trigger, so the 54 world calls fall
   inside it and any call after the tone is refused as stale; `worldPhase()`
   returns the injector's phase for the world and for the rows (zero while
   Warming); the eye shift stays off while owned. Verify in the same flight
   with the census on: the eye draws' measured rows must not move (|leak| <
   1e-6 with a world phase of 1e-4 to 2e-4) while the kind-3 calls' rows carry
   it, kind-5 calls stay 6.0 a frame and injected calls run 54-68 a frame. STOP
   on any |leak| > 1e-5, an injected kind other than 3, or off-thread or
   unreadable calls. Open at the time: the weapon map's phase term, and a role
   for "the ~6 other kind-3 calls a frame". CORRECTED by the stage 2 note
   below: those six are not two other cameras. They are the weapon's own
   refreshes inside the 54 (two per call site, three sites): the old table's
   two other kind-3 cameras (a 90-degree square-aspect one at caller +0x58DE73
   and a 0.236 rad one) were first seen in the cockpit's first frames and do
   not appear in any logged on-foot call sequence.
4. STOP LIST. Late writes 0; no latch line. No `layer did not take` line; the
   layer's refused and lost-draw counts are 0 in every 30 s line. native
   temporal totals `layer_only=28422, layer_only_declined=0`; native sharpen
   `layer_only=28422, layer_only_black=0`: no black eye, no `LAYER-ONLY ... got
   NO composite` line. One mips line (16:20:06.088: 5040x2835, 13 levels, 76.2
   MB), no refusal, not remade across the gaps. No route, layer, mips,
   resolver or census fault line. The releases and the 98-frame episode are
   item 1. The flight plan's `frames-not-treated` stop line was too broad:
   read a release against the scene first.

- ruled out: a hysteresis on the screen-motion naming to keep the route owned
  through the gaps, because the gaps are 1,597 and 929 frames with the world
  paused and no draw naming it; the route would resolve a non-world scene and
  keep the layer re-issuing it.
- ruled out: taking the route's depth identity from H's writers, because the
  resolver's camera rows come from the naming draw's scene constants, and a
  frame with no naming draw has none.
- ruled out: telling the eye cameras from the world's by camera identity (the
  stop note above stands), because one object was an eye camera in the cockpit
  and the world camera on foot; by KIND it is exact: every eye call in 4.63
  million was kind 5.
- ruled out: the tone as the route's trigger in this scene: it is the first
  half-size consumer of H (item 1).

Next flight (none of it built): the same-spot A/B of item 2; a settlement with
the weapon drawn and holstered (the fold-in, the terrain naming path, and the
late-write latch on another chain); the map opened and closed again with Sean
watching the first frames after each handover (the eye route serves 13 frames
before the route owns again). Small instruments that are certain: the RELEASED
line names its last decline reason and the decline log caps per episode, not
per session; a route CPU clock; the fold-in's counters in the 30 s window; the
first treat's duration as its own line; the census reader classifies a camera
by each call's kind.

**Stage 2 implementation note: the world jitter (2026-09-30).** Sean: "Build
stage 2." Built on main 246070a7 from flight 1's census (above): the route now
puts the flat profile's sub-pixel phase into the on-foot world's own kind-3
cameras and resolves the jittered world. BUILT, NOT FLOWN. Environment the fix
depends on: EDVR's native OpenXR runtime on the game process (the camera hook
is the flat profile's detour at EliteDangerous64.exe+0x592200 of flight 1's
build; the RVA is build-specific, a moved hook reads `failed`, the route says
so once and resolves unjittered; under the Oculus native SDK the route never
ran, so this is inert there), Pimax Crystal Super at HMD 0.65 (eye 2620x2533,
output 4032x3898), the 2D screen and H 5040x2835 R11G11B10F, the route at DLAA
on slot 2 (DLSS preset K), fix.ui_quality on, fix.panel_curvature 0, the eye
shift off while the route owns the world (unchanged).

BUILT. (1) The injector in VR (flat_camera_inject.cpp; the pure half is
flat_camera_vr.h, header-only: the role, the admission, the flush, the
counters, the mode word). One detour serves the flat profile, the census and
the route. A per-frame mode word, which the route sets (flatCameraVrFrame,
once a frame on the Present thread from vrWorldRouteFrameBoundary, ahead of
the census's step; flatCameraVrCloseWindow at the trigger), is zero for the
flat profile's whole life and for a process the route never drives, and the
detour then runs the code it always ran (eleven regions of the flat path are
hashed in the rig). Admission is by KIND on every call, never cached per camera
object: it builds on flatCameraAdmit and never spells a kind. Only kind 3 is
ever injected; kinds 4 and 5 (the eyes) are Unsupported, counted and never
mutated; kinds 0, 1, 2 are other kinds. Per call, in order: the off-thread
exit, the kind, the frame window, the role (aspect, near, fov and far are read
only for a kind-3 call the frame could inject), the census hears the call
before any write (told willInject and the role), the flush, the writes (the
flat injector's own: the bound pair, then the flag word's bits 4 and 8, rolled
back if any write fails, the return redirect last), and the call's one outcome
(calls = the sum of the outcomes, in every mode). The entry values come back
after the body (restore-after-call). A camera the route injected and now does
not (a release, a role exclusion, a call after the trigger, a warm-up, a kind
change on a reused object) keeps the phase in its derived blocks, so its first
un-injected call raises the two dirty bits once: the flush, in every mode, the
only write a pass-through or observe-only frame can make. The relay gate closes
only when the detour is quiet (the last frame asked for neither injection nor
observation and no camera waits for its flush; flatCameraVrQuiet), an
injecting frame's window lapses with the flat frame window, and the flat
stand-down rule (eight failed writes in one 5 s window) applies. Rigs:
tools\flat_camera_vr_test, 121 checks (the admission over 2,880 input
combinations against a table written from the brief, the injected-kind
invariant exhaustively, the camera-object reuse scenario as a scripted call
sequence, the one-outcome rule over 227k random calls, the flush for every
reason, the arithmetic bit-equal to the flat injector's) and 102 one-rule
mutants, all caught. The VR path's memory operations on a real camera struct
have never run in the game: they are the flat injector's own statements, pinned
equal, in a new combination (census report, injection, one return redirect and
the restore).
(2) The route (vr_world_route.cpp, vr_world_route_math.h). The window opens at
the frame boundary only when all hold: the key is auto, the route is Warming
or Owned, the gate holds, the layer is live, the A/B key is on, and the frame
that just ended NAMED the screen's source (the naming rule, below). The phase
is the flat profile's own FlatLivePhase: two zero-phase frames (the injector
admits and writes nothing), then the Halton (2,3) sequence, within half a
pixel of the 5040x2835 grid, in render pixels positive right/down, written as a
bound-pair shift of +phase/W and -phase/H (an NDC shift: the same pixel shift
on any field of view). What the resolver gets is the phase a SCENE call
CONFIRMED (jitter and rows, this frame and last), never the chosen one: a frame
whose window was open and whose scene cameras never took the phase resolves
unjittered and says why; a frame part of whose cameras took it (a refused
write) is declined (camera-injection-incomplete) and the phase machine starts
its two zero frames again. The flat runtime's row-pair evidence
(flatCameraCheckRowPair) runs on consecutive resolved frames: two frames' rows
must differ by the phases they claim. vrWorldRouteWorldPhase() answers with the
frame's phase; the eye shift stays off while owned.
(3) The resolver's weapon seam (FlatMonoResolveFrame::firstPersonPhaseMode, one
shader line, three source lines): the map's vector is previous minus current at
the two frames' OWN raster phases, m = (P_prev - P_cur) + (p - c); mode 1 adds
(c - p), mode 0 leaves the vector as given (byte-identical to the build before
the field existed), any other mode rejects attached pixels' history. Proven on
WARP against maps built from explicit positions: error 0 px on nine dyadic
phase pairs, at most 0.002 px on a Halton pair (fp16 rounding), 18 of 18
mutations caught (tools\flat_mono_resolve_test, flat_first_person_phase_gpu_
tests.h); the sign of zero on a real driver is untested.
(4) The instruments and the reader, below.

THE TWO OPEN POINTS, settled from the census call lines (frame 13804, every
kind-3 call's composed rows read as a projection; `edvr_log.py --camera-census`
now prints the same split for flight 1):
(a) THE WEAPON'S PHASE TERM: the weapon takes THE SAME PHASE. The weapon is not
another camera: its calls are six of the 54 kind-3 calls, the same camera
object with a tighter field of view (47.03 against 56.36 degrees vertical) and
a larger near plane (0.0675 against 0.025), the same screen aspect (1.788 from
the matrix, 1.7778 from the rows once the shift is out), two calls a site over
three sites (the draws 4166-4170 and 5922-5923 of the census numbering), and
they refresh whether or not a weapon is drawn. The bound pair is an NDC shift,
so one bound is one pixel shift on any field of view: a first-person camera
given the world's phase rasterises its pixels on the world's jittered grid, the
attached pixels and the world under them share one sample position, and the
map's vector needs exactly (c - p). Left unjittered, every attached pixel would
sit a constant sub-pixel off the one jitter the backend is told, which no
vector can absorb, and excluding the weapon (mode 2 every frame) would throw
its history away every frame. The fold-in stays correct either way: the route
computes the mode from what each frame's cameras actually carried
(vrWorldFirstPersonMode): 1 when the first-person camera carried the world's
phase in both frames (a frame with no weapon map has no attached pixels and
counts the world's phase as the weapon's), 2 when a first-person call was
excluded or refused in either frame, 0 when nothing carried a phase. The 5 s
line's `fp-mode=a/b/c` and the resolver's firstPersonPhaseFrames count the
three. UNFLOWN in substance: flight 1 was in a station with the weapon not
drawn; the settlement leg flies it.
(b) THE "~6 OTHER KIND-3 CALLS": they are the weapon's six (above), inside the
54, not two other cameras (the flight 1 entry is corrected). In frame 13804 all
54 kind-3 calls are one camera object through five view objects, 48 scene
calls and 6 first-person, every one of screen aspect. The table's two other
kind-3 cameras (a 90-degree square-aspect one at caller +0x58DE73, a 0.236 rad
one) were first seen in the cockpit's first frames and appear in no logged
on-foot call sequence (the +0x58DE73 site runs at about 0.2 calls a frame).
The roles, decided on every call (flat_camera_vr.h): a kind-3 call is a
SCREEN VIEW when its aspect is within 4% of H's aspect; of the screen views,
the first-person one has a near plane at least 1.5 times the smallest near a
screen view has shown; both get the frame's phase. Everything else is
AUXILIARY (a probe, a spot light's square camera, a zoom camera): excluded,
counted (`aux=`), its signature (aspect, fov, near, far, caller) logged once
(the first eight), never injected. An unknown role is excluded, never guessed.
The role is aspect-only: any kind-3 camera of the screen's aspect gets the
phase, which the census's per-call role= and inj= will show in flight.
Settlement frames carry about 73-79 kind-3 calls (the world-camera design doc's
estimate): their extra calls are unknown until flown; one with the screen's
aspect is injected as a scene call, one without is excluded and named.

THE NAMING RULE (design-world-camera-motion-2026-09-30.md section 5). A map or
menu frame refreshes about thirty kind-3 cameras and must never pick up the
world's phase. The window therefore opens only after a frame that named the
screen's source (the selector's depthNamed: a pool-family draw into the
screen-sized depth), and stays shut after one that did not, through the
route's three grace frames, until a frame names one again. The frame that
starts cannot be asked: its scene camera refreshes before the draws that name
the source, so the last frame's naming is the only one there is. The cost is
one frame: the FIRST map or menu frame after a world frame has its window open
and its cameras take the phase once (counted `inj-unnamed`, said once per
change in the log, eight lines a session); every later one is shut. Checked
against flight 1: the declines of both paused-world gaps were
`depth-not-screen-motion-source` (2,396 of the 2,399). After a shut frame the
phase machine starts its two zero frames again, so each map costs three
unjittered (still DLAA-resolved) world frames on return. The window also closes
at the route's trigger, and a write on a frame the route had shut is a STOP
(`inj-shut`): the route switches the injector off until its key is flipped.
Warming frames are resolved by the route (the eye route still serves the eyes,
with its own shift on): the resolved H goes back into the game's chain, so the
jitter is not seen; what the eye route does see is the phase in the named
draw's rows for those frames, under half a source pixel.

THE A/B. `experimental.temporal_aa_on_foot_world_jitter` = on (default), dev:
choices on, off, live from the in-headset menu like the route key. off keeps
the route and zeroes the phase: the injector is never stepped, the world
resolves unjittered, flight 1's behaviour; any value but on reads as off (a
typo never writes the game's cameras); `experimental.temporal_aa_jitter` off
stops it too. KEY OFF (`experimental.temporal_aa_on_foot_world` off) is today's
behaviour, pinned: the boundary's one early-return test only gains a flag that
is false until the route first drives the injector, nothing of stage 2 runs
before it, and the injector is never stepped (vr_world_route_test source pins,
the GPU rig's key-off scenario, config_test). Turning the route key off live
passes the injector through until what it wrote is restored, then leaves it
alone; with the census on the detour stays stepped (it is observing).

INSTRUMENTS AND READER. The 5 s route line carries `jitter=` (on, off, idle,
unnamed, no-hook, fault), `phase=` (the world phase in use, render pixels),
`rows=` (what the resolved frame's rows carried) and `fp-mode=a/b/c`; a second
line, `vr world route inject 5s:`, carries the injector's counters
(`inj-scene inj-fp inj-refused warming aux after unsupported other-kind
unreadable off-thread write-fail inj-kinds pair-checked pair-bad inj-unnamed
inj-shut`). The RELEASED line names its last decline and its run
(`(frames-not-treated; last decline: depth-not-screen-motion-source x3)`). The
decline log caps per RUN of declines (three lines, re-armed by a treated frame)
with a session backstop of 64. Route events are one line each: `the world is
JITTERED from frame=` (once per ownership episode), `camera window was open`
(the first map frame, above), `EXCLUDED, not a screen view` (a signature),
`jitter is wanted but no scene camera call was injected`, `camera rows
disagree with the phase`, `the camera hook is not available`, and the STOP
lines. The census (vr_camera_census*.h/.cpp) hears each call before any write,
told `inj=` and `role=` (scene, fp, aux), its 5 s line gains `inj-calls=`, a
sequence header and each eye line gain `phase=X,Y` (render pixels; `-` while
the route is not jittering) and, while the route jitters, only a frame whose
phase is non-zero is sampled (flight 1 spent its whole sample on warm-up
frames). Its key-off path leaves the relay to the route
(flatCameraInjectPause(flatCameraVrQuiet())) and its notes say the census
itself never writes a camera. `python tools\edvr_log.py --camera-census` labels
a camera by the KIND OF EACH LOGGED CALL (the first-seen kind of an object is no
identity), splits one object's projections (flight 1: world camera
0x25FEFC53770, 54.0 calls a frame, 48 at near 0.025 and 6 at near 0.0675, "the
first-person weapon camera's signature"; no "not enough calls" any more), and
ends with the STAGE 2 VERDICT: six lines, PASS, WARN, STOP or n/a each, then
`stage 2 verdict: <word>`: (i) LEAK (the eye rows must not move: |leak| below
1e-6 NDC with a world phase of 1e-4 to 2e-4; STOP above 1e-5), (ii) KIND-3
ROWS CARRY THE PHASE (each injected call's measured shift within 1e-6 of
x = 2 px/W, y = -2 py/H), (iii) INJECTED KINDS (kind 3 only), (iv) OFF-THREAD /
UNREADABLE, (v) ROLES, (vi) INJECTION WINDOW (`inj-shut`, `inj-unnamed`). The
verdict never changes the reader's exit code. Flight 1's log reads (iv) PASS
and the rest n/a (it predates stage 2). The census rigs print the route's two
lines through the route's own formatters, so a format drift fails the build.

FLIGHT PLAN. Frontier, the flight-1 environment; the installed build is the
one `python tools\edvr_log.py --target frontier --expect-build HEAD --version`
names (the coordinator installs). Keys (live `edvr.ini` by the Edit tool, or
the in-headset menu, developer mode, Experimental page):
`experimental.temporal_aa_on_foot_world = auto`,
`experimental.temporal_aa_on_foot_world_jitter = on`, `fix.panel_curvature = 0`,
`fix.ui_quality` on. Turn `advanced.vr_camera_census` ON LIVE only after the
route owns the world and its 5 s line reads `jitter=on` (the census samples the
first frames after it starts; they must be jittered ones), and leave it on.
Read with `python tools\edvr_log.py --target frontier --expect-build HEAD
--grep "vr world route"` (the route lines) and `--camera-census` (the verdict);
note the clock at every change.
1. SAME SPOT, standing still, no menu, weapon not drawn, the same scene in view,
   90 s each, toggled live: (1a) route auto, jitter on; (1b) jitter off (the
   route stays: flight 1's behaviour); (1c) route off (the eye route, the
   baseline); (1d) route auto, jitter on again. Read the 30 s windows that fall
   wholly inside a leg.
   PASS: 1a and 1d: `state=owned jitter=on`, `phase=` non-zero and changing,
   `rows=` equal to `phase=`, `treated` = `frames`; the inject line has
   `inj-kinds=3:N` only, `inj-scene` about 48 and `inj-fp` about 6 a frame,
   `unsupported` about 6 a frame (the eyes, kind 5), and `inj-refused=0
   write-fail=0 off-thread=0 unreadable=0 inj-shut=0 inj-unnamed=0 pair-bad=0`,
   `pair-checked` about the treated frames (`after=` is what is left after the
   trigger: record it); `OWNS the world` and `the world is JITTERED from
   frame=` once per episode; no `declined`, `STOP`, `camera rows disagree` or
   `RELEASED` line; the census verdict reads (i)-(vi) PASS. 1b: `jitter=off
   phase=0.0000,0.0000 rows=0.0000,0.0000`, `inj-scene=0 inj-fp=0
   inj-kinds=none`, owned and treated as in flight 1 (EDVR ~3.5 ms). 1c: no
   route line at all, the eye route's cost (about 5 ms). Cost: EDVR ~ in 1a
   within 0.2 ms of 1b's, application render p50 not worse by more than
   0.3 ms. Sean: 1a against 1b, the world's edges calmer under DLAA with
   nothing swimming; the HUD and the menus unchanged.
   STOP: any `STOP at frame=` line; a census verdict STOP (an eye's |leak|
   above 1e-5, an injected kind other than 3, off-thread or unreadable calls);
   `pair-bad` above 0 (the rows do not carry the phase the route claims: the
   resolver's jitter input is wrong, expect blur; the reader calls it WARN, the
   plan calls it STOP); `inj-refused` or `write-fail` above 0; `jitter=no-hook`
   or `jitter=fault`; a `declined` or `RELEASED` line in a world that did not
   change; 1a's EDVR ~ more than 0.3 ms above 1b's.
2. A SETTLEMENT, weapon drawn and holstered: 60 s holstered, 60 s drawn
   (moving slowly, turning the head), 60 s holstered, then 60 s drawn with the
   jitter key off for Sean's comparison. PASS: as leg 1, and `inj-fp` about 6 a
   frame whether the weapon is drawn or not; with the weapon drawn `fp-mode`
   is mostly mode 1 (the middle number), mode 0 only for the zero-phase frames
   after a restart; `late-hdr-writes=0` (the latch's chain is new here); every
   `EXCLUDED` line names a signature whose aspect is not the screen's. STOP:
   `fp-mode` mostly 2 with the weapon drawn (the first-person camera is not
   carrying the phase: read `inj-fp`, the EXCLUDED lines and the census roles);
   `state=latched`; the weapon shimmering or swimming against the world with
   the jitter on and not with it off (Sean).
3. THE MAPS, on foot: open and close the system map and then the galaxy map,
   30 s each, jitter on, watching the first frames after each open and each
   close. PASS, per opening: one `camera window was open` line and `inj-unnamed`
   up by at most one, at most three `declined` lines, one `RELEASED ...
   (frames-not-treated; last decline: <reason> x3)` (depth-not-screen-motion-
   source, or no-trigger when the map draws no H), the windows `jitter=unnamed`
   or `idle` with `inj-scene=0` and `inj-shut=0` in every window that is all
   map; per closing: `OWNS the world` after 8 treated
   frames and `the world is JITTERED from frame=` once more. The maps look
   exactly as in flight 1: no new shimmer, crawl or doubled lines (the drag
   smear is the world-camera design doc's arc and is expected unchanged).
   STOP: `inj-shut` above 0 or a `STOP at frame=` line (a window opened on a
   frame after an unnamed one, which includes a map frame jittered past the
   first); `jitter=on` in a window that is all map frames; `inj-unnamed` rising
   by more than one an opening.

KNOWN LIMITS. One frame per world-to-map change carries the phase (above). Each
restart (a map, a decline) costs three unjittered DLAA frames. The weapon
fold-in's jittered path is proven on WARP only and unflown. The census must be
turned on after the route owns the world, or its eye budget is spent on frames
with no phase and (i) STOPs with that reason. A settlement's extra kind-3
calls are unknown. The sceneNear anchor never resets, so a context with a
larger scene near would count its scene cameras as first-person (counters
only). edvr_log.py is now about 250 KB: the census reader could be its own
module (not done).

- ruled out: opening the window on the CURRENT frame's naming, because the
  scene camera refreshes before the draws that name the source: nothing exists
  to read when the window must open. The previous frame's naming is what there
  is, and its one-frame cost is counted.
- ruled out: a draw-count or call-count test inside the frame to tell a map
  from a world before the first camera call (the map's fewer draws), because it
  is a guess about scenes: roles and scenes are never guessed here.
- ruled out: excluding the weapon's camera from the phase, because the
  attached pixels would be rejected every frame or sit a constant sub-pixel
  off the backend's one jitter.
- ruled out: an injected phase on Observing frames, because nothing resolves
  them (a frame the route may not resolve is never jittered at the source).
- ruled out: reading "the ~6 other kind-3 calls" as two other cameras, because
  frame 13804 shows them as the weapon's refreshes inside the 54 and the table's
  two other kind-3 cameras never appear on foot.

**Flight 2 of the route (stage 2's first flight), analysed (2026-09-30).** Read
and diagnosed; no code changed. Log `edvr_gfx_20260930_202113.log` (8,721 lines,
20:21:13-20:31:43), v0.18.0-rc.4-132-g80a8cc7d (`edvr_log.py --target frontier
--expect-build 80a8cc7d` exits 0; Sean tagged rc.5 at c7241450, so `describe`
now reads rc.5-17). The same build's first session (`..._201531.log`) has the
route owning 12,872 frames (20:17:47-20:20:11), jitter on throughout, no census;
not analysed further. Environment: native OpenXR over the Pimax OpenXR runtime,
90 Hz, Crystal Super; eye 2016x1949 into a 4032x3898 layer (Elite's
HMDRenderTargetMultiplier 0.500; flight 1's was 0.850); the 2D screen and H
5040x2835 R11G11B10F; the world resolved by DLAA preset K on slot 2 (HDR,
automatic exposure; created once, 20:22:53.517; the DLSS DLL version is not
logged); fix.ui_quality 100, fix.panel_curvature 0, fix.render_sharpness 0.3,
advanced.texture_lod_bias auto, route auto, the jitter key flipped live from the
in-VR menu. SEAN, at a settlement: "seeing some textures shimmer that don't
without it": metal grates, hoses wrapped around a spool, any fine repeating
pattern, his parked ship's fine lines; smooth surfaces fine; "happened even
while standing still"; not with jitter off; "didn't notice any difference on my
weapon".

TIMELINE (log clock). On foot from 20:22:53 (OWNS at frame 11103). Jitter off/on
by Sean: 20:22:56.8/20:23:07.0, 20:23:53.7/20:23:59.5, 20:31:09.0/20:31:19.7,
20:31:26.1/20:31:28.3. RELEASED 20:24:15 after 7,317 owned frames (ship and
hangar on the eye route; OWNS again 20:29:10.8); census on 20:29:47, after the
route owned the world; NumLock draw censuses 20:30:07 and 20:31:00; RELEASED and
re-OWNED 20:30:17-19; the end, a 12-call transition frame at 20:31:39.566 (frame
56693) and RELEASED 20:31:39.8 after 6,708 owned frames. The weapon was drawn
from about 20:30:40 (item 3). Over the 125 route windows: 19,749 frames, 19,443
treated, 16,385 frames in 40 jitter-on windows, 3,144 in 7 jitter-off ones; no
route-off leg (the key stayed auto); 1,444,776 kind-3 calls injected (median 78
a frame, 34-113 by scene); `inj-refused`, `write-fail`, `off-thread`,
`unreadable`, `inj-unnamed`, `inj-shut`, `late-hdr-writes` all 0; `pair-checked`
16,838, `pair-bad` 0; the 162 declines are entry, exit and gap frames; no STOP;
no map opened.

1. THE CENSUS VERDICT (`--camera-census`; 3 sequences, 8 eye draws, from
20:29:47). (i) LEAK PASS: worst |leak| 8.84e-09 NDC at world phases up to 0.3889
px (2.7e-04 NDC). (ii) KIND-3 ROWS CARRY THE PHASE PASS: 234 injected calls
measure the phase given to 7.62e-08 NDC. (iii) INJECTED KINDS PASS (kind 3
only). (iv) OFF-THREAD / UNREADABLE PASS. (v) ROLES WARN. (vi) INJECTION WINDOW
PASS: `inj-shut` 0, `inj-unnamed` 0, 7 JITTERED episodes, 3 RELEASED. `stage 2
verdict: WARN (5 PASS, 1 WARN, 0 STOP)`. The WARN is two understood things:
`inj-fp` 0 (item 3, a defect of the role rule) and one frame, 20:31:39.566, that
wanted a phase and found no scene call (the transition frame before the exit,
resolved unjittered and said so: the last 5 s line's `phase=0.3750,0.0556
rows=0.0000,0.0000`). The world camera 0x28074A12250 makes 78.0 calls a frame
from three sites (26 each) in two projections, all injected as `scene`: 63 at
near 0.025 and 15 at near 0.0675, 1.231 times tighter (the weapon's); the census
note's 92.6 a frame (design 54-68) is the settlement, the design a station. Not
injected: auxiliary cameras (aspect 1.0 at 90 degrees, 0.6118, a 0.149 rad zoom
camera; none the screen's aspect; 37,272 calls), kinds 0 and 1 (21 a frame), the
kind-4 camera (3 a frame) and the eyes (6): so `unsupported` about 9 a frame is
the eyes plus the kind-4 camera, and `other-kind` 21-24 is kinds 0 and 1.

2. THE SHIMMER. What Sean's facts say first: it appears only with the jitter on,
in one session where the mip bias (-1.00), the layer path and the sharpening
(0.3) are the same with it off; it is on fine repeating patterns, not smooth
surfaces; it happens at rest, on his parked ship too. So the cause changes with
the jitter for fine content and is not motion: a bias or a downstream resample
alone cannot make it (the jitter-off legs have both), nor can a camera or
motion-vector error at rest. Ranked, each with what the log shows; the lead was
found last, by reading what the route does with a pixel it will not accumulate.

(f) THE ROUTE'S OWN HISTORY REFUSAL SHOWS JITTERED RAW COLOUR (lead; a
hypothesis, no pixel evidence). The prep refuses history for a pixel whose
engine slot is STALE (a keyed draw wrote it, a later draw changed the depth:
`engineBefore`, flat_mono_shader_source.h:77, everywhere but the 3D main menu,
which the route never is: `f.staticScene = false`, vr_world_route.cpp:323) or
whose record is masked (first seen, gap). For an output pixel whose 2x2 raster
footprint (the four texels around the +jitter sample) holds a refused pixel,
`finishHdr` (:224-236) writes the raw input sampled bilinearly at +jitter
instead of the backend's result: never accumulated, never anti-aliased, and
shown through a filter whose weights follow the eight phases. A 2 px pattern's
amplitude is multiplied by 1 - 2|frac x|: over one cycle 1.0, 0.5, 0.5, 0.25,
0.75, 0.75, 0.25, 0.125 (y: 0.67, 0.67, 0.22, 0.89, 0.44, 0.44, 0.89, 0.22), an
11.25 Hz pulse at 90 fps; smooth surfaces are unchanged. With jitter off the
offset is 0: the raw texel, static. That is each of Sean's facts. And it is the
flat route's own bug of 2026-09-29, found with the camera path and phase exactly
right (rows carried it to 1e-7): the Krait's main-menu hull lines dashed because
the stale-slot rule refused 22.5% of the frame, the plating pair
66DE2CAD/235567BE being unkeyed and overdrawing keyed draws' slots; fixed for
flat by keying it (flat only; VR "stays as it was",
engine_velocity_families.h:35) and a menu-only static policy, Sean "Yep shimmer
fixed" (design-flat-camera-integration.md, "Krait main-menu shimmer"). The VR
route inherits the flat resolver and rule and none of the flat fixes, and until
stage 2 it could not show: with jitter 0 a refused pixel is a static raw texel.

Log evidence that the precondition holds on foot: the engine-motion family lines
name unkeyed pixel shaders "left stock": vs_DE545DC8EE4FBB87 with
ps_A6070F9DD1CFB601 in the on-foot windows ending 20:23:14, 20:23:44, 20:30:14
and 20:30:43 (the family draws 172k-265k a 30 s window), with
ps_91F8937EDA723663 (keyed for flat, not VR) in the ship and hangar and the
window ending 20:31:14, and vs_EB5234DB6ADB491D with ps_B7D50283329322C3 in the
ship and hangar (the line names one pair a family a window and counts none of
its draws). Masked records refuse too (5,601 of 2.18 million in the 20:30:14
window: first seen 453, gap 5,140). No pixel count exists: the route has no
refusal census, the `panel pixels:` counters of the engine-motion line belong to
the eye route's screen-motion pass (one frame in 300, every frame with
`advanced.temporal_aa_diagnostics = 1`; none of 349 logs over 300 KB holds a
non-empty one) and the route ignores `temporal_aa_debug`. The share refused on
foot has never been measured (the flat case was a hull-heavy menu frame).
Against: no unkeyed pair is named for vs_66DE2CAD, the Krait plating's family
(patched ps_864F1F94 only), so Sean's ship is not explained by the pair that bit
the Krait; "any fine repeating pattern" is wider than overdrawn objects (if
plain ground or wall textures shimmer too, (f) is not all of it); and the
weapon's attached pixels take the same raw path (item 3) with nothing noticed,
weak evidence, a weapon being mostly smooth.

(c) MIP BIAS: an amplifier, not a cause by itself. EDVR adds -1.00 to the mip
bias of the game's linear and anisotropic samplers (device_hook.cpp; baked at
device creation, restart to change). `auto` is log2 of Elite's
HMDRenderTargetMultiplier: flight 1's log (161545) reads 0.850 and -0.23, the
three logs after it (18:22, 20:15, 20:21) read 0.500 and -1.00, so the on-foot
textures were 0.77 mip levels sharper in flight 2, the flight with the jitter.
The multiplier is the EYE render fraction (the cockpit's DLSS upscale); the
on-foot screen is drawn at 5040x2835 whatever it is and the route resolves it at
R = D, where EDVR's own rule (log2 of the fraction) is 0.0. The log's check
("they agree, so the mips are right for this frame") compares with the eye
fraction and cannot see that. NVIDIA's DLSS guide gives log2(R/D) - 1 (from
memory; the SDK docs are not in the tree): -1.0 at R = D, so the value in force
is what NVIDIA recommends for DLAA, not an error by that rule. The same-session
toggles say it is not the sole cause (jitter off at -1.00 is calm); it hands the
accumulator, and (f)'s raw pixels, more sub-pixel energy to fail on. A leg at 0
sizes it.

(e) DOWNSTREAM OF THE RESOLVE: an amplifier too. The layer takes the 5040x2835
screen into the 4032x3898 eye layers by trilinear from a mipped copy and RCAS
(0.3; its ini text says it "makes fine-line shimmer worse") follows, with no
temporal filter at the eye grid while the route owns the world (the door is
layer-only: eye-takes = door-layer-only = 38,838). A resolved H holds more fine
detail than the jitter-off one and head micro-motion resamples it. The
jitter-off legs share the path, so it cannot start the shimmer; it can scale it.
Leg: fix.render_sharpness 0, live.

(g) NGX'S INTEGRATION OF FINE HDR CONTENT, not separable here: eight Halton
phases at 90 Hz repeat at 11.25 Hz (`kTemporalJitterCount` 8, NVIDIA's minimum
at R = D, from memory), preset K with IsHDR and AutoExposure, pre-tonemap
specular on metal. The flat route runs the same call and sequence. The same
jitter into another backend (fix.temporal_aa on, then fsr, both live) shows
whether it is NGX; the same shimmer in all three puts it upstream.

(b) REGISTRATION: cleared at every link the log exposes. Phase written (census
headers, frames 2-4: (-0.375,-0.0556), (0.125,0.2778), (-0.125,-0.2778)) against
rows measured: 7.62e-08 NDC over 234 calls, where a flipped sign or no removal
reads 0.5-1.1 px. The route's pair check reads the NAMING DRAW's scene constants
as the game last wrote them (`engineVelocitySourceCameraRows`, b1 rows
270..275), not the camera call's output: 16,838 consecutive pairs differ by the
phases they claim, 0 bad, at 2e-6 NDC. The prep's cancellation replayed on the
real census rows (a scratch replica of the shader arithmetic) leaves
0.0003-0.0013 px of camera-term motion at rest (frames 2->3, 3->4), against
0.50-0.56 with no removal, 0.67-1.11 flipped in y, 0.50-1.00 flipped in x,
0.25-0.28 at half scale; the repo's rig proves the same on WARP. The float the
injector was given is the float NGX gets (`worldApplied` -> `f.jitterX/Y` ->
`InJitterOffsetX/Y`, dlaa.cpp:740, unmodified): the eye path's and the flat
route's call, content displaced right/down by +jx/+jy (NDC +2 px/W, -2 py/H;
Unreal's). No history churn: 4 resolver reset events in the route's 8.8 minutes
(first treat 20:22:53.5; camera cut 20:23:37.5, 216.9 m; re-entries after 295.8
s and 1.735 s); `scene-resets` 84, all in the 17 idle ship-and-hangar windows
(the eyes' detector), none in an owned window; the slot-2 feature created once.
Unseen: NGX's own acceptance of history. `phase=` and `rows=` on the 5 s line
are not independent (`rows=` is the phase the frame applied); frame 56693 is the
one place they differ.

(d) ENGINE-RECORD MOTION CARRYING THE PHASE: no evidence; the record path
applies the same `unjitterRow` to the engine snapshots EN and EB, and at rest a
static record's motion is the camera term. (a) UN-JITTERED DRAWS: no evidence,
one direct check. The camera rule holds every source pool draw to the naming's
camera: 1,437,786 checks in nine windows, 0 declines on other scene constants or
unseen rows; the 8,626 declines are 3,284 draws before the naming
(20:23:44-20:24:44) and 5,342 of vs_AACFDCF2FB9AD809, a weapon family
(weapon_motion.cpp), whose rows 270..273 differ at 0.000 m from the naming's:
the weapon's projection. So pool families (props, machinery, ships) and the
naming draw read the injected rows. Unchecked: terrain, non-pool draws, the
weapon family's own buffer. The NumLock censuses cannot help: census_offscreen
is 0 and census_cb_watch empty, so each logs the 6 eye-composite draws (VS
5C36AF05, PS CFE84157, 2016x1949) and about 450 copies and 310 dispatches, no
world draw; they say nothing about Sean's ship.

RANK: (f); (c) and (e) as amplifiers; (g); then (b), (d), (a) (cleared or no
evidence). (f) fits all four facts and has a precedent here; its weakness is
that nothing counts pixels. The rest is judgment, not measurement.

NEXT FLIGHT, no build. Same spot, standing still, the grates or the ship in
view, 45 s a leg; Sean scores the shimmer 0-3. Ask first: does a fine pattern on
plain ground or a wall shimmer, or only objects? ((f): objects only; (c): all.)
L1 baseline, route auto, jitter on. L2, for (f):
experimental.temporal_aa_on_foot_world off (the eye route) with
advanced.temporal_aa_debug motion_source and advanced.temporal_aa_diagnostics 1,
all live: are the shimmering structures yellow (stale slot) or red (masked
record) and not green or blue? The 30 s `engine motion` line should then print
`panel pixels:` shares (`--grep 'panel pixels'`). The eye route handles stale
pixels its own way (the ini says yellow takes the camera's motion), so this
classifies pixels, not the route's output. L3, for (g): route on,
fix.temporal_aa on, then fsr, jitter on (the prep and its raw fallback are
shared, so (f), (c), (e) are unchanged by it). L4, for (e): fix.render_sharpness
0. L5, for (c): restart with advanced.texture_lod_bias 0, then 0.5, route auto,
jitter on and off. Predictions: (f) structures yellow or red, shimmer less at L4
and L5; (c) all fine patterns including ground, gone or much less at L5; (g)
gone in L3's other backends. L2 and L5 settle the top two.

3. THE WEAPON. `inj-fp` is 0 in every window because the role rule cannot tell
the weapon's calls from the world's: it keys on the camera struct's NEAR
(flat_camera_vr.h:41-51, at least 1.5 times the smallest near seen; read at
flat_camera_inject.cpp:120, 427-430) and the struct's near is 0.025 for the
weapon's calls too (the census camera line of the object's first call, a
weapon-projection call: near 0.025, fov 0.8203). The 0.0675 of the flight 1
entry and the stage 2 note is the composed rows' near, another number: a spec
error of mine that the rule inherited (decision (a) stands, its input was
wrong). The weapon's 15 calls a frame (five groups of three adjacent calls, one
per site) were injected as `scene` and carry the phase to 7.6e-08 NDC: the
weapon IS jittered with the world, as designed. But the route needs
`firstPersonInjected > 0` to say so, so `vrWorldFirstPersonMode` answered 2
(refuse attached pixels' history) on 12,563 of 12,869 weapon-mapped frames in
jitter-on windows (97.6%), 306 in mode 0, never mode 1. The map is bound
whenever weapon motion runs; the weapon was drawn from about 20:30:40 (373
frames of the family's declines in the 20:30:44 window, 2,160 in the next). Mode
2 is consistent: attached pixels carry reject=1, so finish shows the raw input
at +jitter, registered, filtered, not accumulated, motion 0 for a weapon locked
to the screen. It is the same raw resample as (f); Sean saw nothing. The
jittered fold-in (mode 1) is still unflown. Fix, not built: classify
first-person by the struct's fov (0.8203 for the weapon against about 0.98 for
the eyes and scene; field +0x280, already read), or drop the role from the mode:
every screen-view call carries the same phase, so "one injected, none refused"
is the claim it needs.

4. COST. No route-off leg exists and the jitter-off stretches (10.1, 5.8, 10.7,
2.2 s) sit inside 30 s census windows; the native benchmark windows (whole-frame
GPU and CPU p50), which end at every settings change, resolve them. One spot
(20:23:43-20:24:17): jitter on 8.0 s, off 1.7 s, on 15.1 s read GPU p50 / CPU
p50 7.382/1.698, 7.598/1.718, 7.433/1.706 ms: jitter on costs no more than off
(the off leg is 0.2 ms higher, 153 frames) and the injector's CPU, about 80
calls a frame, is not measurable. The 20:31 spot (on 1.7, off 5.3, on 2.8
seconds): 10.051/4.884, 9.329/3.897, 9.622/4.235, a drift that follows where
Sean looked, not the leg. EDVR ~ (GPU census, 30 s): wholly owned with jitter on
3.281 (20:23:44: door 0.587, world resolve 1.888, mips 0.056, world layer 0.050,
layer composite 0.307, sharpen 0.263; application render p50 8.40), 3.094
(20:29:44) and 4.532 (20:30:14, the heaviest view: application render 11.48,
game 6.95); with a 1.7 s release 3.735 (20:30:44); with 5-6 s of jitter off
2.812 (20:24:14) and 3.487 (20:31:14). The world resolve is 1.59-1.95 ms in
every owned window. The eye route in the ship, for scale: 5.1-5.8 ms (door
3.7-4.3, upscaler 2.5-3.0). The plan's gate (jitter on within 0.2 ms of off) is
met by the same-spot pair. Not measured: a route-off leg at the settlement,
which would price the route's gain.

- ruled out: a sign, scale or unit error in the jitter handed to the backend,
  because the rows measure the phase to 7.6e-08 NDC, the prep cancels it to
  0.0013 px on the real rows, 16,838 consecutive naming-draw row pairs agree and
  the call is the flat route's own.
- ruled out: a history reset or a phase pairing offset each frame, because there
  were 4 resolver resets in the route's 8.8 minutes, all at state changes, no
  scene reset in an owned window, the slot-2 feature was created once and no
  consecutive pair disagreed.
- ruled out (pool families): draws reading a camera without the phase, because
  1,437,786 checks held to the naming's rows and the only "another camera" is
  the weapon family.
- ruled out: the bias alone or the layer path alone as the cause, because Sean's
  jitter-off legs in the same session share both and are calm (they stay
  candidates as amplifiers).
- ruled out: the NumLock censuses and the `rows=` field of the 5 s line as
  evidence about world draws, because with census_offscreen 0 the censuses log
  the eye-composite draws only and `rows=` is the phase the frame applied.

FIX PROPOSALS, nothing built. Nothing ships on (f) until a leg or a census shows
the shimmering structures are refused pixels (AGENTS.md: no fix on an untested
hypothesis).

- (f) 1. The decisive build, small: a dev key that sets `staticScene` for the
  route (vr_world_route.cpp:323), the flat menu's stale-slot policy applied on
  foot (every stale pixel takes the camera term; flat 1B, valid there on 1,822
  of 1,822 stale pixels), live, default off = today, named on the 5 s line (a
  name that says what the user gets). One flight with the key toggled at the
  grates: the shimmer gone with it on settles (f); the cost is that a moving
  object drawn by an unkeyed pair ghosts while it is on. With a refusal census
  and view (per-cause pixel counts on every 90th frame from the prep, in the 5 s
  line, and a live view painting refused pixels in H, yellow stale and red
  masked) it also sizes the share.
- (f) 2. The root-cause fixes, only if (f) holds: key the pairs the log names
  for VR (91F8937E and 235567BE already have flat keys; A6070F9D and B7D50283
  need their bytecode captured with `glare_shader_dump` and the harness first);
  and replace "stale means refuse" on foot with a depth-validated camera term: a
  stale pixel takes the camera term, accepted only when the previous frame's
  depth at the reprojected position matches the expected depth within the 1% the
  TAA path already uses (`taa()`, :173-177). A moving unkeyed object fails the
  test and is refused as today; a static one is accumulated. That removes the
  class and not just the named pairs; the route needs its own previous depth for
  it.
- (c) The world's bias belongs to the world's own render fraction. While the
  route owns the world the samplers that draw it want EDVR's rule at R = D
  (0.0), not log2 of the eye fraction. A bias is baked at sampler creation, so
  the proposal is two variants of each game sampler (the creation hook holds all
  67) and a choice where the game binds samplers for the pass; no bind-time hook
  was found, so its hot-path cost is measured first. Stopgap if L5 removes the
  shimmer: advanced.texture_lod_bias 0 for on-foot play, at the cockpit's cost.
  The route's 5 s line should name the bias in force.
- Weapon: item 3. Every key off stays exactly as today.

**Stage 2 experiment build: one flight to settle (f) against (c) and to test
the fix direction (2026-10-01).** Sean approved ("Go", relayed by the
coordinator) the build the flight-2 analysis proposed. Five items and nothing
else; out of scope by decision: a route-aware mip bias, keying the named pairs,
the depth-validated camera term. Environment: VR, Frontier install, native
OpenXR over the Pimax runtime (flight 2's headset, eye 2016x1949 into a
4032x3898 layer, HMDRenderTargetMultiplier 0.500), H 5040x2835 R11G11B10F
resolved by DLAA preset K on slot 2, automatic exposure, DLSS DLL version not
logged. Every new read sits inside the route key's auto branch: the flat
profile never runs it (flat coverage is a feasibility note at the end, not
built).

EVIDENCE SINCE THE ANALYSIS (Sean and the coordinator, 2026-10-01).
1. Sean, asked whether plain ground and walls shimmer: "not everything
   shimmered, only certain textures". Selective by texture leans (f), a
   property of the draw, over (c), which would touch every fine pattern alike;
   it does not rule (c) out (a bias shows only on textures with fine detail),
   so both legs stay.
2. The shimmer reproduces in FLAT: Epic, v0.18.0-rc.4-114-g20031385, flat HDR
   route at 3840x2160 (R = D), DLSS, on foot, standing still, log
   `edvr_gfx_20261001_051141.log`: the blue hose spools and the bright top edge
   of a metal fence. The F10 audit at 05:17:25 saved 11 unknown-projection
   pairs (22 stages).
3. The flat legs in `edvr_gfx_20261001_052230.log`, same spot: the copy route
   still shimmers (before_post off 05:24:55-05:25:02, the 5 s line shows HDR
   treated=0); TAA (on) and FSR shimmer too (05:25:15-05:25:49, short legs); AA
   off (05:25:19.9-05:25:47.2) is calm; fix.render_sharpness 0.7 to 0.0 at
   05:25:55 "changed the pattern of the shimmer, but still shimmered".
4. THE ROUTE OWNS IN HMD CINEMA AT THE MAIN MENU (Frontier, 80a8cc7d, log
   `edvr_gfx_20261001_053139.log`; Sean set the game's 3D mode to HMD Cinema
   and fix.vscreen_res_width = 4032). The 5 s line reads state=owned layer=live
   gate=held jitter=on, phase = rows, door-layer-only = eye-takes, and target
   and hdr both 4032x2268: the route follows the width (H is the screen's size,
   not 5040x2835). Sean: "some lines shimmering on my ship, but not all". He
   took an eye dump at 05:32:44 (FinalCrisp L/R 02-15 crops and overviews, the
   route owning the frame); the route RELEASED at 05:32:52 (no-trigger x3) as he
   left.
   The reading (coordinator): the flat Krait hull-line case in VR. The flat
   menu's stale-slot-takes-the-camera-term policy fixed it in flat on 09-29; the
   route keeps `staticScene` false. It also fits flat on foot shimmering
   (`staticScene` false there) and AA off being calm everywhere. Consequence for
   the plan: leg A runs first at the main menu in cinema mode (a static scene,
   repeatable, no settlement trip) and again at a settlement as the second
   check. Checked: nothing in the key, the census or the view is gated on the
   journal or on foot: the keys are read at the boundary inside the auto
   branch, the census and view fields are set in treatWorld, and the only gate
   the route has (uiLayerWorldScreenHeld) held at the menu, which is why it
   owned there.

- ruled out: (c), the mip bias, as a NECESSARY cause, because flat shimmers
  with every sampler at bias +0.00 (one at +1.00) in the texture-filtering
  census. In VR the -1.00 can only be an amplifier; leg B measures how much.
- ruled out: (f) through unkeyed pairs as the FLAT cause, because `flat engine
  motion unkeyed 5s: live=1 binds=0 distinct=0 pairs=[]` at 05:17:26, the
  window of the F10 audit, with the shimmer on screen. The pairs VR flight 2
  named (vs_DE545DC8EE4FBB87 with ps_A6070F9DD1CFB601 and ps_91F8937EDA723663;
  vs_EB5234DB6ADB491D with ps_B7D50283329322C3) are a VR-route fact this does
  not touch.
- ruled out: (h), resolving in HDR before the tone, as the cause, because the
  copy route (after the tone) shimmers the same.
- ruled out: (g), a backend-specific cause, because TAA and FSR shimmer too;
  and AA off is calm, so the raw image is steady and the shimmer is made by the
  jittered resolve path the backends share.
- ruled out: (e), the sharpen pass, as the cause, because 0.7 to 0.0 changed the
  pattern and left the shimmer; an amplifier only. VR leg A's last step repeats
  it.
- open, the lead: (f'), the shared prep's refusal of a pixel's history (a stale
  engine slot, or a record whose history has a gap) and the raw jittered input
  the finish shows there. It is shared by every backend and both routes, and
  on foot `staticScene` is false in flat as in VR. Flat counted 3,861
  engine-motion history gaps in the window at 05:15:49 (2,360 of 9-64 frames,
  604 over 64), so in flat the MASKED class may matter more than the stale one;
  VR flight 2's first-seen and gap masks were 5,601 of 2.18 M records in 30 s
  (0.26%). This build measures both in VR: stale against masked.

THE BUILD, piece by piece. Each piece: what it is, its log fields, what the
log shows if the code never ran, and its key-off contract.

1. STEADY DETAIL, the live A/B for (f).
   `experimental.temporal_aa_on_foot_world_steady_detail = off | on`, default
   off, beside the jitter key, developer tier (`# dev: choices on, off`), so
   the in-headset menu's Experimental page flips it live exactly as Sean
   flipped the jitter key in flight 2. Read at every boundary while the route
   key is auto. On: a pixel whose engine slot a later draw overdrew (the slot's
   depth bits are not the pixel's) takes the camera term instead of refusing
   history: `FlatMonoResolveFrame::staticScene`, the flat 3D menu's policy
   since 09-29 (Krait menu shimmer), set in treatWorld by the line that was
   `false`. STALE SLOTS ONLY. Masked records (first seen, or after a gap),
   corrupt slots, the sky and the weapon's attached pixels stay refused. The
   masked path COULD be the shimmering pixels (geometry that popped in or
   changed record; flat's 3,861 gaps); the census's `masked=` and the view's
   red answer it, and the key is deliberately not widened to them. A moving
   object drawn by a shader the engine table does not name can ghost while it
   is on (the flat menu has nothing moving). The route's 5 s line carries
   `steady-detail=on|off` after `fp-mode`, and one line says `steady-detail is
   ON|OFF from frame=N` at the first boundary that reads the key and at every
   change. If the code never ran: no token and no line (an older build).
   OFF CONTRACT: with it off, the census off and the view off, the resolver
   gets flight 2's inputs (`staticScene` false, no class texture, the `debug`
   constants zero, the shader's branches not taken). Pinned by the route rig's
   source pins (read once, inside the auto branch after the early return,
   assigned in one place, cleared with the route's state), by config_test
   (shipped default off equals the code's fallback, with a control mutation; a
   flat profile reads it off), and by the resolver rig on WARP (on: the 16
   stale pixels forgiven, the 4 masked, corrupt and sky pixels still refused;
   off: all 20 refused, as before).
2. THE REFUSAL CENSUS. Gate: `advanced.vr_camera_census` (existing, live; no
   new key). On the resolves it samples (one in four of those that ask, so a
   window holds about 110) the prep writes one class byte a pixel into a
   private R8 texture (low seven bits what the pixel is, bit 7 refused), a
   `census` kernel sums the refused pixels by class in group-shared counters
   (16 stripes of atomics), and a ring of four staging slots reads the sums back
   with DO_NOT_WAIT (a full ring drops the sample: `dropped=`). A third line
   follows the route's two every 5 s window:
   `vr world route refusal 5s: census=on every=4 treated=N asked=N sampled=N
   read=N dropped=N size=WxH pixels=N refused=N refused-pct=X stale=N masked=N
   corrupt=N sentinel=N unreprojectable=N camera=N range=N depth=N weapon=N
   other=N forgiven=N steady-detail=on|off view=on|off`. `pixels` is what the
   read-back samples examined, so every share is of pixels; `forgiven` is the
   stale pixels the key sent to the camera term (so the stale share stays
   measurable with the key on); `weapon` is attached first-person pixels the
   weapon's map could not place. "Ran, 0 refused" is pixels > 0, refused=0.
   "Never ran" is no line (the census key off, the route not engaged, or an
   older build), or treated=0 (not owning the world), or asked=0 with
   treated > 0 (the key reached the route and not the resolver), or read=0
   (nothing came back): the reader names each. GPU cost: not measurable on
   WARP; one dispatch and the class write on a quarter of the resolves. Measure
   it live: census key off then on at one spot, `world resolve` in the EDVR GPU
   census (the camera census's own observer is CPU and outside that section).
3. THE VIEW. `advanced.temporal_aa_debug = motion_source`, live, while the
   route resolves: the HDR finish paints the prep's class in the eye path's
   colours before the game's tone pass (each colour scaled by the pixel's own
   level, so the hue survives the tone and the absolute colour does not): green
   joined, red masked, blue pool surface (camera term), yellow stale slot,
   magenta corrupt, orange stale stamp, cyan first-person, white any other
   refusal, dim no engine slot. It paints the CLASS, not the refusal: with the
   key on the yellow pixels are the same pixels, now forgiven. One line says
   `the refusal view is ON|OFF from frame=N`; the 5 s lines say `view=on`. The
   eye path's own motion_source view is untouched. Never ran: no line, and the
   picture is the world's.
4. THE WEAPON'S ROLE. Flight 2's `inj-fp` was 0 because the rule keyed on the
   struct's near plane, 0.025 for the weapon too. First-person is now a
   screen-aspect call whose struct fov is at most 0.92 of the widest fov any
   scene call of the SAME frame showed (flight 2: 0.8203 against 0.9831, ratio
   0.834); the near test stays. Relative, not 0.8203 hard-coded: nothing shows
   the game fixes the weapon's fov whatever the player's FOV setting is, and a
   frame-local anchor never lets a zoomed scene camera turn next frame's scene
   calls into first-person. The price: a weapon call before the frame's first
   scene call counts as scene, and flight 2's frame opens with three, so 12 of
   its 15 weapon calls are recognised and 3 are counted scene (same phase, only
   a counter). The inject line now ends `fov=<narrowest>..<widest>` (radians;
   `fov=-` no frustum read), so the log says whether the struct carries a
   second field of view. The fold-in's rule is unchanged: a drawn weapon is
   credited the world's phase only when a first-person call was injected and
   none refused; otherwise mode 2 as before. Expected after the fix, weapon
   drawn: `inj-fp` about 12 a weapon frame, `fov=0.82..0.98`, `fp-mode=0/N/0`
   with N about the weapon frames. MODE 1 IS UNFLOWN. What marks it wrong: (a)
   `fp-mode` mode 1 with `pair-bad` above 0 (the rows do not carry the phase the
   fold-in adds); (b) `inj-fp` about as large as `inj-scene` (a wider
   screen-aspect camera set the anchor: the `fov=` range shows it; the reader
   WARNs); (c) `inj-fp` 0 with the weapon drawn (the reader WARNs and says
   whether the struct has two fields of view or one); (d) the refusal line's
   `weapon=` not falling against a mode-2 window; (e) Sean seeing the weapon
   swim against the world with jitter on and not off while the log says mode 1.
5. THE READER, `edvr_log.py --camera-census`: parses the refusal line, the
   steady-detail and view lines and the inject line's `fov=`; prints a refusal
   section (per window: key state, view, treated, samples, share refused and
   its cause mix, forgiven, with the route's `fp-mode`, `inj-fp` and fov beside
   it; totals by key state; findings: the contradictions above, never-measured
   windows, drops, unnamed causes); extends (v) ROLES with the weapon rules (a)-
   (c). The census rig regenerated its fixture and a glue rig reads the route's
   own formatted lines through the real reader; `--self-test` fixtures cover
   each finding, with negative controls.

EVIDENCE FROM THE RIGS (no flight). The full build is green: 101 rigs in 121 s,
187 s end to end, the config contract at 231 keys read and 231 documented. The
resolver rig on WARP (new flat_refusal_gpu_tests.h) holds the HLSL class
numbers to the header's, no resources when nothing asks, the census counts to
the rejection mask's popcount (stale 16, masked 1, corrupt 1, sky 2 of 20),
the key's forgiven count, eight asking resolves giving two samples, reset
frames never asking, and the view's colours (yellow, magenta, white, blue,
dim, red, green; nothing with it off). The route rig (207 checks) holds the key
parse, every line's format, "ran, 0 refused" against "never ran", the buffers
and the source pins. The camera rig's R12 runs flight 2's 78-call frame (12
first-person, 66 scene; the anchor not carried; a zoomed frame all scene; the
fov range), and its mutation proof catches all 22 new mutants (119 in all).
The reader's self-test has a negative control for each finding. NOT measured:
the census's GPU cost (WARP cannot time it) and anything in a headset.

THE BIAS KEY. `advanced.texture_lod_bias = 0` means no bias override (the
game's own samplers stand; device_hook.cpp reads it with the other texture
keys, 0 with 0 anisotropy switches the whole override off). It is read at
device creation: a restart. The ini line is commented out (`#texture_lod_bias =
auto`); the flight sets `texture_lod_bias = 0` under [advanced].

FLIGHT PLAN (Frontier, a settlement, flight 2's settings: eye fraction 0.5,
curvature 0, UI quality on, route auto, jitter on; install by
tools\install_edvr.py; `edvr_log.py --target frontier --expect-build HEAD
--version` first, then `--camera-census`). Turn `advanced.vr_camera_census` on
live AFTER the route owns the world (legs A, B); leg C's weapon numbers are on
the always-on route lines and need no census key. Sean scores the shimmer 0-3
at one grate, standing still, 20 s a step.
A. Live, one spot, first at the MAIN MENU in HMD Cinema (fix.vscreen_res_width
   4032; look at the ship: static, repeatable, nothing moves but the camera, so
   the key's ghosting caveat cannot bite), then at a settlement grate standing
   still. A1 key off (baseline). A2 key on. A3 off. A4 on. A5 key off,
   `advanced.temporal_aa_debug = motion_source`: THE DECIDING TEST is whether
   the view paints exactly the shimmering textures yellow or red (supports (f))
   and the calm ones green or blue, or not (against it). Sean names the colour
   of the ship's shimmering lines (or the grate) and of something calm. A6 view
   off, fix.render_sharpness 0 (flat already says: pattern changes, shimmer
   stays). If the in-headset menu cannot be opened at the main menu, the same
   keys are plain ini lines the route reads at every boundary (live).
   (f) confirmed: A2 and A4 calm, A1 and A3 shimmer; the refusal line at A1
   shows `stale=` as the larger cause (the shimmering area's share or more; at
   the menu the flat Krait case read 22.5% of the frame) and at A2 `stale=0`
   with `forgiven=` about A1's `stale=`; A5 yellow on the ship's lines or the
   grate. (f') by the MASKED class: A2 unchanged, `masked=` the large cause,
   A5 red on the grate; the next build would relax the gap fallback, not
   built. (c) alone: A2 unchanged, A5 green or blue (accepted pixels), B
   calm. Neither: A2 unchanged, A5 green or blue on the shimmering textures,
   refused share a few percent (sky, range), B no calmer: the cause is on
   ACCEPTED pixels (the camera term's reprojection, the phase pairing, the
   backend's accumulation) and this build cannot separate those.
B. Restart with `texture_lod_bias = 0`, census on; the same spot (the menu's
   ship again, or the grate): B1 key off, B2 key on, 20 s each; the log's
   texture-filtering line must say bias 0 (the bias is baked at device
   creation, so it applies at the menu too). (c) as
   an amplifier: B1 much calmer than A1, B2 the calmest. Not an amplifier: B1
   as A1. Flat shimmers at +0.00, so a calm B1 is not expected if (f') holds.
C. Weapon: 30 s holstered, 30 s drawn, key off. Pass: `inj-fp` 0 holstered and
   about 12 a weapon frame drawn, `fov=0.82..0.98` drawn (one value holstered),
   `fp-mode=0/N/0` drawn and no `pair-bad`, (v) PASS with `mode 1:`; the census
   `weapon=` below a mode-2 window if one is available. Wrong: (a)-(e), item 4.
COST. Census key off then on at the A spot: `world resolve` within 0.1 ms of
off is cheap enough; the view's cost is the finish's one extra read while on.

WHAT THE VR LEGS CAN AND CANNOT TELL. They can separate STALE from MASKED from
neither at the shimmering textures (the view) and size each (the census), and
the key tests the stale half as a fix. They cannot test (h) or (g): the VR
route resolves in HDR with one backend and has no copy route to compare, and
the flat legs above ruled both out without a headset. A "neither" lands on
accepted pixels. The cheap additions that would discriminate there, proposed
and NOT built: (1) a flicker census: keep the previous resolved output on the
sampled frames and sum |out(N) - out(N-1)| by class and by luminance bucket
(with the camera still, that is the shimmer itself, attributed to refused or
accepted pixels and to bright or dark ones); one extra R11G11B10F copy
(about 57 MB) and a compare dispatch on a quarter of the resolves; (2) log the
exposure the backend is given (HDR flag with automatic exposure, pre-exposure
1; dlaa.cpp passes InPreExposure 1, InExposureScale 1) so a bright-pixel
difference has a number to read.

FLAT COVERAGE (feasibility; NOT built). The resolver is shared, so the three
instruments can run on the flat route on foot: both call sites in
flat_runtime.cpp (the copy route and the HDR route, `f.staticScene =
flatFrameThroughMenuCopy(...)`) take one OR and two assignments, and the 5 s
window that prints `flat menu HDR copy:` is where the census is taken and its
line printed (the same formatter with a `flat route` prefix). The view paints
in the HDR route only (the resolver paints when `hdr`), which is the default on
foot. The catch is the flat allowlist (runtime_profile.h): the VR census and
view gates are `advanced.vr_camera_census` and `advanced.temporal_aa_debug`, and
the latter, with `advanced.temporal_aa_diagnostics`, has readers across the
tree (the FSR context's AMD debug flag, UI and engine-velocity diagnostics, the
eye path's debug views): allowlisting them wakes all of those in flat. So
either the flat route reads the gates through one explicit accessor like
`requestedTemporalMode()` (no new key, a config.cpp addition, config_test pin),
or flat gets its own allowlisted keys (new keys: Sean's call). Cost: files
runtime_profile.h (one key: the steady-detail one), config.h/.cpp (accessor),
flat_runtime.cpp (about 45 lines), the refusal formatter moved into
flat_mono_refusal.h, edvr_log.py (the section on a log with no census lines,
plus a fixture), flat_temporal_test and config_test pins; one full build
(about 3 min) and one --dll-only promotion, and the flight is at Sean's desk.
Risk: low to moderate; flat_runtime.cpp is production for Epic testers, but
every key defaults off and the resolver's off contract is already rig-pinned.
The VR key-off contract does not change (the VR route's pins are separate and
its lines must stay byte-identical: the route rig and the census fixture hold
them). A smaller first step: only the steady-detail key (one allowlist line,
two OR lines, a token on the flat 5 s line): it settles the stale half of
(f') at the desk; the census and view are needed only if it changes nothing.

KNOWN LIMITS. The census samples a quarter of the resolves: a window's share
is an estimate (every sample is a whole frame). The view's colours are
scaled by the tone pass. Class 'joined' and 'not-rig' pixels are accepted and
not counted (the contended counters cost more than the answer). The role test's
first weapon group of a frame is counted scene. Mode 1 is unflown. GPU cost is
unmeasured. A screen-aspect camera wider than 1.087 times the scene's fov would
turn the scene's calls into first-person: the `fov=` range and the reader's
check (b) show it.

FLIGHT 3, 2026-10-01 05:49: (f') CONFIRMED. Frontier, 02c1c456, log 054902;
the game's 3D mode HMD Cinema, the main menu with the ship in view,
`fix.vscreen_res_width = 4032`, the route owned at 4032x2268, census on.
Steady-detail went ON at 05:50:05, OFF at 05:50:22, ON at 05:50:28; the view
went ON at 05:50:40. The one whole key-off window (05:50:28) refused 5.674% of
the pixels, all but `range=` of it `stale=` (58,105,650 of 58,111,623). Every
whole key-on window refused 0.001-0.003% (`range=` only, `stale=0`) and
forgave about 58 M pixels, the same ~5.7%. Sean: the shimmer on the ship's
lines stops with the key on ("That fixed it!"). With the view on he took an
eye dump: the fine YELLOW (stale-slot) lines are the ones that shimmered, so
the deciding test (the view paints exactly the shimmering lines) passed as
well as the A/B. So the shimmer is the prep's
stale-slot refusal (a later draw overdrew the pixel's engine slot) showing
the jittered raw input. Flat on foot runs the same rule (staticScene false),
which explains the hoses there. Not yet checked: movers (the key gives an
overdrawn slot the camera term, so a moving object drawn that way can ghost),
the settlement leg, flat. Proposed next (Sean's call): a depth-validated
camera term for stale slots in the shared resolver (keep history where last
frame's depth at the camera-reprojected spot matches, refuse otherwise), so
flat on foot gets it too; then a settlement check with movers and a desk
check in flat before any default changes.

SETTLEMENT FLIGHT, 2026-10-01 (log 060011): the blanket form of the key
(02c1c456: every stale slot takes the camera term) works on foot at the
settlement. Sean: "it looks beautiful". He reported no mover ghosting, but he
did not look for it (people walking, a ship landing, doors), so the caveat is
unchecked, not cleared.

HMD CINEMA ON FOOT (CORRECTED in FLIGHT 4 below: an arrival spell, not
Cinema; reported with it; recorded only, not in scope here): the
route declined every frame (about 450 a 5 s window, gate=no; reasons
`depth-not-screen-motion-source` and `engine-views-unavailable`), so it never
owned the world, and the whole panel ghosted and flickered. That is a separate,
pre-existing cinema-mode problem, not the steady-detail key's, and nothing in
the entry below widens to it.

2026-10-01, THE DEPTH-VALIDATED STEADY DETAIL, VR AND FLAT (built and
rig-proven; NOT FLOWN). The proposal at the end of FLIGHT 3, built
as one rule in the shared resolver, one key, two readers.

THE CHECK (flat_mono_shader_source.h, the prep). A pixel whose engine slot
holds another depth than the pixel (a later draw overdrew it: "stale") takes
the camera term, as every pixel without a slot does, and then has to pass one
test: last frame's depth, in the best of the four texels around the position
the camera term sends the pixel to in last frame's raster, must be within
max(1e-6, 1% of expected) of `expected`, the depth the camera term says this
surface had there. The position is the camera term's true previous uv times
the render size, plus the previous raster phase (jitter.zw), minus half a
texel; the four texels are the bilinear footprint, clamped to the image. Pass:
the camera term stands (not refused; class stale, counted `stale-kept`). Fail:
refused exactly as with the key off (counted `stale-refused`). Masked,
corrupt, sentinel and sky pixels and the weapon's never reach the test, and
the 3D menu's blanket policy (staticScene) wins and does not run it.

THE TOLERANCE AND WHY (a throwaway simulation, numbers below; no flight).
- Depth is reversed-Z float32, infinite far: d = near / z, sky 0. The float32
  spacing is 0.7e-7 to 1.2e-7 of d at every range from 0.025 m to 1e8 m, so a
  relative error in d is the same relative error in z at any distance: the
  tolerance is relative. The floor 1e-6 equals 1% at d = 1e-4 (z = 250 m with
  near 0.025 m); farther than that the floor is the tolerance (4% of d at
  1 km, 40% at 10 km). The resolver's own TAA applies the same 1% and 1e-6 to
  its history depth in taa() today, so the two cannot disagree.
- Edges and thin lines, the shimmer's own geometry (a static line, the jitter
  phases of consecutive frames): the nearest texel keeps a 1 px line 62.5%
  (0 deg) to 74% (45 deg) of the time and a 2 px line 81-87%, so it would
  refuse the very pixels this exists for; the four texels keep every line of
  1 px or more 100%, a 0.5 px line at 30 deg 100% (the nearest: 44.5%) and at
  45 deg 80.5% (53%). A 0.5 px line at 0 deg keeps 25% in every footprint.
  3x3 is no better on lines and leaks farther at edges.
- Slanted planes (a static plane, relative depth change per pixel g): with
  four texels at 1%, g = 1% keeps 100%, 2% 99.9%, 3% 91%, 5% 67%. g is 2% per
  pixel at a 1 deg grazing angle at VR 5040x2835 (2.6% flat 3840x2160), 0.2%
  at 10 deg. At 0.3% a 1% plane keeps 99.8% and a 2% one 58%; so 1% is the
  loosest that does not start refusing ordinary floors.
- Movers: a face moving in depth by 0.5-1% a frame is kept, 2% a frame is
  refused: at 5 m and 90 fps that is 9 m/s, so only fast depth movers are
  seen. A flat-faced lateral mover 40 px wide at 1 px a frame keeps 98.75% of
  its stale pixels (3x3: 99.2%): its two edge columns are refused and its
  interior passes, as under the blanket form, carrying a history that is
  misregistered by the step. HONEST LIMIT: the check refuses edges,
  disocclusions and surfaces that changed depth; it cannot see a surface
  that slides sideways at constant depth. Also at an edge the best of four
  keeps the trailing 1 px of a surface that moved away (the background beside
  it was there a texel over): the rig pins it (9 of 16 refused, 7 kept).
- ruled out: the nearest texel alone, because it refuses 25-93% of static
  thin lines; 3x3, because it is no better on lines and leaks farther; 0.3%,
  because a plane at 2% a pixel (a 1 deg grazing floor) keeps only 58% there;
  3%, because a face moving 2% a frame in depth is then kept.

THE KEY OFF CONTRACT. `experimental.temporal_aa_on_foot_world_steady_detail`
keeps its name and values; off is the default and refuses a stale slot as
before, byte for byte: the prep's arithmetic and every output are what they
were (the resolver rig's recorded key-off hashes still pass, unchanged), no
second depth image is made, the backend is handed the same image as always,
and the depth-check frames count zero. "on" now means the depth-checked form.
The blanket form is gone from the VR route (the route rig pins that it never
sets staticScene); it survives for the flat 3D main menu alone, untouched.

PREVIOUS DEPTH AND ITS COST. EDVR's TAA already keeps last frame's depth. DLSS
and FSR keep none, so the first frame with the key on makes a second R32_FLOAT
image at the render size (57 MB at 5040x2835, 33 MB at 3840x2160) and from
then the depth the backend is handed alternates between the two images: the
prep writes this frame's depth into the one that is not last frame's and reads
last frame's as t8. No copy and no extra write: memory only. A frame with the
key off writes the first image as before, so the key can flip live. If the
image cannot be made the log says so once and the frames run as with the key
off (`depth-check` counts them as skipped). GPU: only a stale pixel pays, four
4-byte loads and a few dozen ALU ops; with 5.7% of a VR frame stale (flight 3)
that is 0.8 M pixels, 13 MB of reads, about 0.03-0.05 ms at 5040x2835; the
flat Krait menu's 22.5% at 4K would be about 0.06 ms. An estimate: WARP cannot
time it. The flight reads `world resolve` ms on the route line key off against
key on.

FLAT USES THE SAME KEY. runtime_profile.h: one allowlist line. flat_runtime.cpp:
steadyReadKey once a Present (off unless the file says "on", any case), the
state handed to the resolver at both treatment sites (the copy route and the
HDR route) right after the menu's `f.staticScene` lines, which are unchanged
(four mentions, none from the key; the flat temporal rig pins it). The log says
`flat runtime: steady-detail is ON|OFF from frame=N (...)` when the key
changes (and at startup when it is on; nothing while it is off) and, every
5 s while a temporal mode runs, `flat steady detail 5s: steady-detail=on|off
depth-check=RAN/SKIPPED`. No flat census or view (the stale share on foot in
flat stays unmeasured). Backends: DLSS, DLAA, FSR and EDVR's TAA all take the
check.
A flat-specific key is not needed. Flat's ini is edvr-flat.ini:
  [experimental]
  temporal_aa_on_foot_world_steady_detail = on
VR's is edvr.ini, the same two lines (the line is already there, off).

THE VR CENSUS (`advanced.vr_camera_census`). The refusal line's `stale=` is
now `stale-refused=` (refused: with the key off every stale pixel, on the
ones the check refused) and `forgiven=` is `stale-kept=` (not refused: the
camera term, confirmed by last frame's depth); new `depth-check=RAN/SKIPPED`
(resolves with the key on whose prep ran the check, and those that could not;
a reset frame is neither). The line now also prints while only the key is on.
`edvr_log.py --camera-census` reads both spellings, totals each key state
alone, prints stale-kept as a share of all pixels and, with the key on, of
the stale pixels, and WARNs on: key on and no depth-check frame at all, key on
and the check never ran, key off with stale-kept or checked frames; one note:
key on, the check ran and kept nothing.

EVIDENCE FROM THE RIGS (no flight). Resolver rig (WARP, the new
flat_steady_depth_gpu_tests.h), through DLSS's stub, FSR's and EDVR's TAA: key
off refuses a stale block and makes no second image; key on keeps it in a
still scene and the backend is handed THIS frame's depth through eight frames
with the key flipped on and off; a surface that arrived is refused and kept
once it has been there a frame; 0.9% and 1.1% each way and the floor (1e-5
deep, 5% inside it, 20% outside); a 1 px line one texel over is found in both
axes and both signs of the previous phase, two texels over is not; a camera
three pixels over reads the depth where it sends the pixel; a camera moved
along its view axis compares with `expected`, not the pixel's depth; the
menu's blanket policy keeps a stale pixel whatever the depth says and the
check neither runs nor counts; a reset frame refuses all and counts nothing
and the next frame has its depth; corrupt, sentinel, sky and masked stay
refused with the key on; the census splits 16 stale pixels into kept or
refused; through the TAA a kept pixel reaches its history (122 and 134) and a
refused one takes the current colour (64 and 192). 22 mutants of the shader
are each caught (the tolerance at 0, 0.5% and 5%, the floor at 0 and 1e-3, min
for max, no check, inverted, the wrong position, the pixel's own depth for
`expected`, no previous phase, the wrong sign, the current phase, no half
texel, one and sixteen texels, this frame's depth, the blanket form, key
refuses, sky and corrupt kept, a refused pixel counted as range). The route
rig, the census rig and its fixture, the glue rig (real formatter into the
real reader, a key-on window), the reader's self-test (both spellings),
config_test (the key off in the shipped ini, both readers' fallbacks, the flat
allowlist) and flat_temporal_test pin the rest. NOT measured: the GPU cost;
the real DLSS and FSR SDKs given alternating depth images (the rig's backends
are stubs that read the same textures); anything in a headset or at the desk.

UNSURE. (1) The SDKs take the depth resource per evaluate call, so alternating
should be inert, but the stubs cannot show it; a quality change at the key
flip with `depth-check` ran > 0 would be the sign, and the key is the revert.
(2) Lateral movers: the check cannot see them; flight 2 below tests it.
(3) Flat's stale share on foot is unknown, so "kept" there has no number
until a flat census exists.

FLIGHT PLAN (install by tools\install_edvr.py; `edvr_log.py --target <t>
--expect-build HEAD --version` first; VR legs `--camera-census` with
`advanced.vr_camera_census = on`, flat legs `--grep "steady"`).
1. VR, the main menu, HMD Cinema (fix.vscreen_res_width 4032), key on: calm as
   with the blanket key (flight 3); the census `stale-kept` about 5.7% of the
   pixels (flight 3's stale share) and `stale-refused` near 0; `depth-check`
   ran in about every frame of a window (450) and skipped 0.
2. VR at the settlement, standing still, then with movers (people walking,
   a ship landing, doors), key off then on, twice: static detail calm with
   the key on; no ghost trailing a mover (Sean looks at one mover on purpose,
   twice); the stale-refused share rises where something moves; `world
   resolve` ms key off against on.
3. Flat, Epic, at the desk (edvr-flat.ini above, temporal_aa on, any
   backend, on foot): hose spools and hull plating calm with the key on,
   shimmering off; the 5 s line says on with `depth-check` ran about every
   frame; the menu is unchanged (key on or off).
Pass: 1 calm and nearly all stale kept; 2 calm with no ghost; 3 calm. If 2
ghosts, the check is too loose for movers and the answer is a motion
signal, not a tighter depth tolerance (ruled out above).

PROPOSED, NOT BUILT. (a) A near-miss counter: stale pixels refused by less
than twice the tolerance, so the tolerance's edge has a number from a flight.
(b) The flicker census of FLIGHT 2's plan, if the shimmer outlasts the key.
(c) A flat census and view, if the flat stale share is wanted: the VR formatter
moved into flat_mono_refusal.h and a gate that is not
advanced.temporal_aa_debug (its readers across the tree make that key unsafe
to allowlist in flat). (d) A flat-specific key name: not needed.

**The vscreen auto-fit (2026-10-01; BUILT, NOT FLOWN).** Sean: "we're rapidly
approaching making route on the default for everyone". With the route running,
5040x2835 (auto's 125% of the 4032 eye) is render cost nothing needs: the game
draws every on-foot pixel of it (G-buffer, depth, HDR, HUD), the route resolves
and mips it, and the eye shows about 3500 of it. Flown with the route on: 4032
wide "looked fine", 3504x1971 "looks great still" (Frontier, 02c1c456, log
060703). `fix.vscreen_res_width = auto` now fits the screen to what the eye
shows whenever the route will run and keeps today's rule otherwise. Explicit
widths are exact; the flat profile never arms any of it. CALIBRATED 2026-10-01
(THE CALIBRATION, below): auto is 3504 on Sean's rig, the width he chose.

THE RULE (`src\common\vscreen_fit.h`, pure; `tools\vscreen_fit_test` runs the
very code the DLL runs).
- The route will run, at launch, when ALL hold: `experimental.
  temporal_aa_on_foot_world` is auto (its default since 2026-10-01); the curve
  is NOT a condition (the curved route dropped it, below: the route re-issues a
  curved screen, and `routeStandsAsideForCurve` is gone); the UI layer is live
  (`fix.ui_quality` on, a temporal mode on, the jitter switches as shipped:
  the layer's own `uiLayerNotLiveReasonFor`); the runtime is EDVR's OpenXR.
  That last one cannot be read at device creation (the module list is empty
  until openvr_api.dll is called, 1.2 s later), so none-loaded-yet is
  undecided, not a failure: the eye width on record is only ever written by
  EDVR's runtime. Elite's native Oculus back end, or a foreign openvr_api.dll,
  fails it. Else: today's rule, unchanged: roundTo16(1.25 x the eye).
- Fitted width = roundTo16(clamp(m x A, 2880, cap)), 16:9, m = 0.70 (2026-10-01;
  it was 1.0), A the screen's head-on footprint in eye pixels, cap = the legacy
  width (a fit never asks for more than the old rule did), nudged off
  1920x1080 and 3840x2160 (the only 16:9 sizes the game's own targets took in
  four flights' logs; the world-screen gate and the panel recognition key on
  the panel's size). A small eye whose cap is under 2880 gets the legacy width.
- A = fraction x eye width / d. The fraction is the screen's head-on width as a
  share of the eye's at panel distance 1.0: the distance override scales one
  float of the composite's placement (its z translation), so the footprint
  varies as 1/d, and the stored fraction rescales to a changed
  `fix.panel_distance` or eye width with no new measurement.
- Nothing measured yet: the seed is a footprint, 5006 px at 0.7 on a 4032 eye
  (0.8691 of the eye at distance 1.0), which is Sean's chosen 3504 divided by m,
  so the FIRST launch on his rig is 3504x1971. (Until 2026-10-01 the seed was the
  width itself with m = 1.0; measured, the screen spans 5000-6100 px on that rig at
  0.7, so a measured footprint at m = 1.0 would have fitted 5040.)

THE LINE. `vScreen resolution: auto = N wide: rule=fitted|legacy source=seed|
measured|none route=run|no eye= distance= legacy= [footprint= m= floor= cap=
clamp= nudged=] -- prose`, m printed to three decimals (the reader recomputes the
width from it). It names the rule, the numbers it was made from and, for legacy,
EVERY route condition that failed. An explicit width prints
`vScreen resolution: explicit N wide`. The in-headset menu's hint says the same
(about 70% of the screen's width in your view).

THE CALIBRATION (2026-10-01; BUILT, NOT FLOWN). Sean: "for my current openxr
resolution 3504 should = auto". His rig: Pimax Crystal Super, eye 4032 px,
`fix.panel_distance` 0.7, curvature 0.3. The measurement is trustworthy: it repeats
across sessions, follows 1/d to 1.1%, and is inflated about 6% by head pose (the 15
on-foot windows with 12 or more samples in Frontier 082459 and 100155, normalised to
0.7: window medians 5000-6135, median 5267; the lowest sample of each window 4651-5175,
median 4951; the stored session medians 0.889 and 0.939). So the plan's m = 1.0
could not give 3504 (5267 px against 3504), and the reader's SHAPE STOP was the
check's fault, not the instrument's: head pose inflates the height at first order
(+2.3% per degree of yaw) and the width at second order, the eye's pixels are square
(1894 against 1893 px per unit tangent, so 16:9 is the right reference), and the 10746
px "window 10" was ONE sample, which alone drove the 102% "between windows" figure.
DECIDED (Sean's 3504; the overseer's choice of estimator): store the 10th percentile of
the session's on-foot widths (the head-on floor, `kFootprintQuantile`) instead of the
median, which removes the pose inflation and most of the scatter (a median file gave
3408 or 3600 at m = 0.665 from session to session). A window logs its median and
range, not its samples, so p10 cannot be read back from a log; it is bracketed from
(min, median) of each window: an exponential skew above the floor gives 4991, a
half-normal one 5000, 4985..5002 for 30 to 60 samples a window. Working value 5000
px at 0.7, so m = 3504 / 5000 = 0.7008, kept as 0.70, and the seed footprint is
3504 / 0.70 = 5006 px (0.8691 of the eye at distance 1). The rig pins m 0.70, seed
5006, chosen 3504; the auto width from the seed on a 4032 eye is 3504 at 0.7, 3776 at
0.65, 4096 at 0.6, 3072 at 0.8, 2880 from 0.85 up (the floor); a 2064 px eye is the
legacy 2576 either way. The first session's own p10 replaces the seed, and
CALIBRATION says how far apart they are (within 5% passes).
ruled out: the instrument as the cause of the SHAPE STOP, because the aspect reads
1.55-1.80 only through head pose (the median shape is -5% to -7% off 16:9).
ruled out: the curved composite as the shape cause (the instrument reads the game's
flat quad and curvature 0 shows the same spread), clipping (the corners come from
unclipped arithmetic), and "other" samples mixing in.
ruled out: m = 1.0 with the median, because the median is pose-inflated and noisy
(5121-5409 across two stored sessions) and 3504 is 0.665 of it, not 1.0; and m =
0.665 with the median, because it still scatters 3408-3600 session to session.
ruled out: a seed left at the width (3504), because m x 3504 = 2453 floors at 2880.
An OLD stored file (written when the median was stored) is never read as a p10:
the record carries `est=p10` and `parseRecord` refuses one without it, so Sean's
file (`fraction=0.889105`, a median; honoured it would fit 3584 at 0.7 and 3856 at
0.65) reads as no record and the seed applies until a session on foot measures a
p10. The reader's changes are under THE READER.

THE INSTRUMENT (`src\d3d11\vscreen_footprint.cpp`). At the 2D screen's
composite (vs 5C36AF05, ps CFE84157; after the game's own issue) the quad's four
corners go through the composite VS's own arithmetic
(`docs\shaders\composite-vs.asm`): cb0 rows 9..11, cb1 rows 270..273 (the clip
columns), the four vertices (stride 20, at the draw's base vertex) and the
per-instance SIZE (a float2 slot, at the start instance). Horizontal NDC extent
/ 2 = the share of the eye's width. Twice a second one draw's four sources are
copied into one 256 byte staging buffer; the Map is at the frame boundary three
frames later, DO_NOT_WAIT, so the render thread never waits. Its D3D calls step
past EDVR's hooks (the scope is taken outside the fault guard), a budget of 5
faults stands it down, and a source that is not what the measurement assumes
(stride not 20, a buffer too small, no SIZE slot, a negative base vertex)
skips the sample and is counted by reason. Armed: VR profile, key auto.
ruled out: an occlusion query as the instrument, because it counts AREA (a
width follows only by assuming the screen's shape), is clipped where the
screen overflows the eye (d under about 0.6), and needs GPU-latency handling;
the corners are exact, free on the GPU and clip nothing, and each source was
already read in flight (panel_curve's SIZE and cb0, the census's cb1 rows).
The 30 s line: `vscreen footprint 30s: window= samples= on-foot= other=
skipped= late= [why=reason:n,..] draws= distance= applied= eye=WxH fp= frac=
range=lo..hi h= shape= [other-fp=] at1= frac1= session-n= session-frac1=
persisted= fit= legacy= m= floor=`. fp: the on-foot median in eye pixels;
shape: its pixel aspect, 1.778 when the corner arithmetic and the eye size
agree; at1/frac1: at panel distance 1; session-n/session-frac1: the session's
on-foot p10 (the head-on floor) at distance 1; persisted: what the file holds;
fit: what the next launch fits if the route runs. Menu and on-foot samples are kept
apart (the layer's world-screen gate, else the journal's); only on foot is
stored, from 12 samples up. If it never ran there is no `vscreen footprint`
line at all, not even `vscreen footprint: armed --`; armed with no composite
seen prints draws=0; seen but unreadable prints skipped= with why=.

PERSISTENCE. `edvr_logs\vscreen_auto_footprint.txt`, beside
`vscreen_auto_eye_width.txt`: `fraction=0.869097 est=p10 eye=4032 distance=0.700
samples=177`, the session's on-foot p10 at distance 1.0, rewritten when it moves
0.2%. A garbled or implausible (outside 0.05..3.0) file is no measurement, and so
is one without `est=p10` (a median, written before 2026-10-01). A save is a temp file
(`vscreen_auto_footprint.txt.tmp`) moved over the file in one write-through step and
is acknowledged (`persisted=`) only when the whole record reached it; a failed save (a
locked destination, a directory in its place) leaves the old file exactly as it was,
logs `vscreen footprint: SAVE FAILED (Win32 error N)` (three a session), adds
`save-failed=N` to the 30 s lines and is retried at the next window even when the p10
has not moved (release review R2, confirmed on main 73e02a7b; the reader's STORED
WARNs on the token). A build before this printed `persisted=<value>` whether or not
the write happened.

THE READER. `python tools\edvr_log.py --target frontier --vscreen-fit
--expect-build HEAD`: RULE (the width recomputed from the line's own tokens, to
16 px, and what the panel patch applied), INSTRUMENT, ON FOOT, STABLE (the windows'
distance-normalised medians within 8%), SHAPE (the median window shape against
16:9 x fx/fy, from the graphics log's `native benchmark workload: ... game FOV
radians` line, else square pixels: PASS from -9% to +3%, WARN to +-15%, STOP beyond),
DISTANCE LAW (frac1 within 2% across distances), CALIBRATION (the stored p10 against
the seed footprint, 5% at a 4032 px eye), STORED. ON FOOT, STABLE and SHAPE use only
windows with 12 or more on-foot samples. A log from a build before the calibration
(its windows print m=1.00 and stored a median) reads CALIBRATION n/a and STORED
"MEDIAN" rather than a false PASS. On Sean's two older logs: 100155 STABLE PASS 7.3%,
SHAPE PASS -5.1%; 082459 STABLE WARN 18.3% (head turning), SHAPE PASS -7.1%. The
census verdict's NDC figure read a literal 5040x2835; it now takes the route lines'
hdr=, else the JITTERED line, else the panel patch's size, else says it does not
know.

GATES. `tools\vscreen_fit_test` (293 checks; `mutants.py --run`: 147 mutants of
the header, the state writer and the wiring pins, all caught),
`tools\vscreen_footprint_glue_test` (57 checks: the real glue on WARP with the four
sources at their real offsets, the stored file, the defective sources, the fault
budget, a skewed session whose p10 is 0.67 f where the median would be 0.85 f, a
locked destination in the first persisting window, then the real reader over its
log), `config_test`
pins the resolver's route-key fallback to the shipped default, the reader's
self-test, `tools\vscreen_fit_fixture.log` held to the formatters.

NOT KNOWN, and which line reads it. (1) The p10 itself on Sean's rig: it is
estimated from the windows' minima and medians (THE CALIBRATION), and the first
session after this build measures it (`session-frac1=`, CALIBRATION). (2) That the on-foot and menu footprints agree (`other-fp=` against
`fp=`). (3) The 1/d law (`frac1=` across a leg at another distance). (4) The
quad's corners at +-1 with SIZE in a float2 slot, from panel_curve's flights
(`shape=`). (5) What a narrower screen costs elsewhere: it sizes every
screen-mode target, the intro movie's, HMD Cinema and the FSS chrome (213/320 of
the width: 3408x1917 at 5120).

FLIGHT PLAN (Frontier or Epic; one on-foot spot, standing still 60 s a leg, so
two 30 s windows). Every leg, `edvr.ini`: `[fix]` `temporal_aa = dlss`,
`ui_quality = 100`, `panel_curvature = 0`, `panel_distance = 0.7`; `[experimental]`
`temporal_aa_on_foot_world = auto`. Install by `tools\install_edvr.py`; the
first line of every read is `edvr_log.py --expect-build HEAD --vscreen-fit`.
A. Delete `edvr_logs\vscreen_auto_footprint.txt`, `vscreen_res_width = auto`.
   Expect `rule=fitted source=seed route=run ... auto = 3504 wide`, applied
   3504x1971, the armed line, then windows with `on-foot` above 0, `skipped=0`,
   `late=0`, `persisted=` a number. Exit.
A2. Restart, same ini, no edits. Expect `source=measured` and the width the
   measurement gives: roundTo16(0.70 x the stored p10) (3504 when the p10 reads
   5000-5012 px at 0.7). CALIBRATION is the verdict on m. This is the leg that
   matters. (The old stored file is a median and is ignored; delete or rename it
   for a clean leg A anyway.)
B. Restart with `vscreen_res_width = 5040` (explicit; the cost leg): the same
   spot and view. Compare the `EDVR GPU census:` lines (the world resolve, the
   mips, the world layer, ms a frame) and the `native benchmark:` windows
   (whole-frame GPU and CPU p50, which end at every settings change) with A2's,
   and Sean's eye on the text: 3504 against 5040.
C. Restart with `panel_curvature = 0.3`, auto: expect `rule=legacy route=no`,
   5040x2835, the prose naming `fix.panel_curvature is above 0`, and no route.
D. (cheap, same spot) Restart at `panel_distance = 1.0`: the target near 2450
   under the 2880 floor (clamp=floor), `footprint=` near 3500, DISTANCE LAW PASS:
   frac1 matches A2's.
PASS: A shows the fitted seed line and a stored measurement; A2's CALIBRATION is
PASS (else Sean picks 3504 or the measurement); STABLE and SHAPE PASS; C names
the failed condition. FAIL: no `vscreen footprint` line (never ran), draws=0 (the
composite not recognised), skipped= large (a source is not what it assumes,
why= says which), SHAPE off 16:9 (the corner arithmetic or the eye size is
wrong), a width in the rule line that its own tokens do not give.

FLIGHT 4, 2026-10-01: the depth-validated steady detail PASSED, key on, in
flat and VR (85119ce9; Epic log 071356, Frontier log 074129).
- Flat, Epic, at the hose spools: `flat steady detail 5s: depth-check=
  383..435/0`, so the check ran on every resolve and skipped none. Sean:
  "Flat looks great".
- VR main menu (owned 07:42:19-07:42:45, explicit 3504x1971): stale-kept 40-44 M
  a window against stale-refused about 1 k; refused-pct 0.000.
- VR on foot (owned 07:43:20-07:43:59): refused-pct 0.008-2.3%, stale-refused
  42 k-623 k against stale-kept 61-167 M a window. The check refuses a few
  (movers, disocclusions) and keeps the rest. Sean: HMD Cinema looks good at
  the main menu and on foot, and normal HMD looks good on foot. Movers were not
  looked at on purpose.
- CORRECTION to "HMD CINEMA ON FOOT" above: the decline is not specific to
  Cinema. Both sessions open on-foot play with a declined spell straight after
  loading in (all `depth-not-screen-motion-source` or `engine-views-
  unavailable`), then the route owns:
  - 060011: 06:00:58-06:02:00, about 60 s.
  - 074129: from the journal's LoadGame at 07:42:53 to 07:43:20, about 27 s.
  ruled out: "the route declines every frame in Cinema on foot", because
  074129 owned in Cinema on foot after the arrival spell.
  Open: what ends the spell (the arrival camera? the first on-foot source
  frame?).
  CORRECTED 10-01 by the Phase 1 build's log reading
  (design-world-camera-motion-2026-09-30.md, 8.6). In the spell the layer took
  every screen draw: 060011 reads "5174 2D screen draws asked, 0 left in the
  picture" and "5396 ... 0 left". The luma probe's final stage is black at
  every sample of both spells; the world arrives at once afterwards.
  ruled out: "the ghost Sean saw was the eye route serving the panel during
  the spell", because no eye-route panel was shown in it.
  What the ghost was is unknown. Cinema on foot looked good in 074129.
- Next: steady detail default ON for VR and flat (the key-off pins move with
  it), shipped with Phase 1 and the auto-fit.

STEADY DETAIL DEFAULT ON, VR and flat (2026-10-01). Sean: "Please make it
default". `experimental.temporal_aa_on_foot_world_steady_detail` now falls
back to on in both readers (the VR route's readExperimentKeys and the flat
runtime's steadyReadKey) and the shipped edvr.ini says on; the name, the
section ([experimental]) and the values are unchanged (moving it to [fix]
would be a rename, Sean's call). Why: flight 4 passed with it on (flat 071356:
the check ran on every resolve, "looks great"; VR 074129: the menu kept 40-44
M stale pixels a window against about 1 k refused, on foot and Cinema looked
good), and Sean reports "I did not see any issues with ghosting or trails" in
his flights since 85119ce9. That is informal: he did not run a movers leg, so
it is the evidence behind the flip and not a measurement of ghosting.

EXPLICIT OFF is today's refusal exactly. The readers compare the file's word
with "on", so "off", a typo and an empty value all read off: the prep refuses
every stale slot as before the key existed, no second depth image is made and
the depth-check frames count zero. The pins are config_test (the ini and both
fallbacks agree; the flat scope and the VR profile read an explicit off; the
control now flips a fallback to off), the route rig (the parse of the word,
the ini line), the flat temporal rig (the reader's comparison with on), and
the resolver rig's recorded key-off hashes, unchanged. A mutation of each (a
fallback back to off, any word but off reading on, the ini back to off) fails
one of them.

SCOPE. The route stays default off (the route's own key,
`experimental.temporal_aa_on_foot_world`), so in VR the new default matters
only while the route is auto; in flat every temporal mode on foot now gets the
check unless edvr-flat.ini says off. The 3D menu's blanket policy is
untouched. An existing ini with no line now reads on; one with an explicit off
keeps it. The VR refusal line is now printed while only this key is on, i.e.
in every route session, with census=off and the depth check's frame counts:
edvr_log.py reads such a window as `census-off` (summarised on one line, no
WARN) instead of a census that never measured. A flat session that starts on
says so once at startup; one that starts off says nothing (its 5 s line has
steady-detail=off).

THE CURVED ROUTE (2026-10-01; BUILT, NOT FLOWN). Sean flies
`fix.panel_curvature` 0.3 with `panel_distance` 0.7 ("the only thing missing is
panel curve"). The route stood aside for any curvature above 0 (item 7 of its
first entry above): the curve substitution swallows the game's screen draw and
the layer's re-issue repeated the game's draw. Now the re-issue repeats the
STRIP.

THE MECHANISM (`src\d3d11\vscreen.cpp`, `panel_curve.cpp`). After
`panelCurveSubstitute` has drawn the strip into the eye, and when the route owns
the frame (the layer planned a re-issue: `worldReissue.on`),
`curvedScreenSwallowed` runs screen motion's recognition (the flat tail does, so
the naming stays alive) and `worldScreenReissueCurved` opens the layer's own
Begin, issues `panelCurveReissue` and closes it with End. ONE helper,
`drawStripHeld`, saves the game's vertex buffer, index buffer and topology,
binds the strip and issues the draw through the original pointer; the
substitution and the re-issue both call it, so they cannot differ. What the
layer's draw shares with the game's: the strip's buffers (the same objects), the
draw's arguments, the game's vertex shader, its placement constants (the
re-issue sits inside `beginPanelOverride`'s bracket, so the panel-distance
override's constants are still bound), its pixel shader, its SIZE slot and its
rasterizer state. What differs is what the flat re-issue also differs in: the
layer's target, the viewport and scissor through the layer's map with the jitter
cancelled, the opaque blend, and the route's mipped resolved screen with a
trilinear sampler at PS slot 0. RCAS stays after the composite (the layer-only
door's order, unchanged). So the bend and the placement are the game's at any
curvature and any panel distance by construction; `tools\panel_curve_test`
proves the shared input-assembler state, arguments and bytes over a table of
curvatures, columns, signs and gains, and that a live curvature change rebuilds
before the next re-issue. The strip's own screen-motion pass is skipped for a
frame the route owns, as the flat tail skips it; a re-issue the layer refuses
leaves that eye to the eye route without that frame's screen motion, as for a
flat screen.

THE STATES. `pending`: the first composites after the key or the curvature comes
on, while the substitution reads the panel's SIZE (about 50 ms, then it builds
the strip): the game draws its flat quad and the layer re-issues the flat quad
to match (the plan accepts a substituted draw, the draw path falls through to
the flat tail), so both are flat and consistent. `stood-down`: a fault in the
substitution or in the strip's re-issue stands the feature down for the session
(the first fault, as it always did): the game's quad is drawn again and the
route keeps owning with the flat re-issue. A fault in the strip's draw after the
layer's Begin closes the bracket without taking the eye
(`uiLayerWorldReissueEnd(.., false)`, counted as a fault refusal): the eye route
serves it.

THE LOG. The route's 5 s line gains, right after `steady-detail=`, `curve=` and
`curve-reissues=`. `curve=off` while the game's own quad is drawn, `pending` and
`stood-down` as above, else `C/S/G`: the curvature (a fraction of a circle), the
strip's columns and the depth gain in the panel's model units, the numbers of the
strip in hand, which the layer's draw uses too. `curve-reissues` is the strips
the layer drew in the window and equals `eye-takes` on a healthy curved owned
window; `eye-takes` above 0 with `curve-reissues=0` under a `C/S/G` curve would
be the layer drawing a FLAT screen under a curved game draw. The OWNS line ends,
by state: `off`, nothing (byte-identical to before); `C/S/G`, `; the screen is
curved (curve=C/S/G): the layer draws the same strip the game's own draw is
substituted with, so the bend and the placement are the game's`; `pending`, `;
the screen is set to curve (curve=pending): the strip is not built yet, so the
game and the layer both draw the flat quad until it is`; `stood-down`, `; the
screen is set to curve but the curve stood down (curve=stood-down): the game
draws its own flat quad and the layer re-issues it flat`. (The first build of
this entry gave pending and stood-down the C/S/G sentence, which was false for
both; found by the rig's author and fixed before any flight.) `panel curvature:
the VR world route's layer drew the same N-column strip ...` prints once, at the
first re-issue. The stays-off line no longer names curvature and the layer's
`curved-screen` refusal is gone (a stale build is the only way to read one).

THE AUTO-FIT. Its curvature condition is dropped: `routeStandsAsideForCurve` and
`RouteFacts.curved` are deleted from `src\common\vscreen_fit.h` (its header said
to, when this landed) and `vscreen_res.cpp` no longer reads the curve. The
route's conditions at launch are three: the key auto, the UI layer live, EDVR's
runtime (and the flat profile never fits). With curvature on, auto gives what it
gives flat: `rule=fitted`, from the stored footprint or the seed (3504 at
`panel_distance` 0.7 on a 4032 eye), `route=run`. Before this build it gave
`rule=legacy route=no`, 5040 (leg C of the auto-fit's own flight plan above,
now superseded).

THE FOOTPRINT, from the code and arithmetic. (a) The instrument could not see a
curved composite at all: its call sat in the tail of the game's own issue, which
the substitution swallows, so with curvature on it would have printed `draws=0`
(its own flight plan reads that as "composite not recognised") and stored
nothing. (b) It is now called from `curvedScreenSwallowed`, after the
substitution has put the game's vertex buffer back, so it reads the quad the game
bound: the screen's footprint as if it were flat. (c) That is the quantity the
rule's m is defined at, texels per eye pixel at the MIDDLE of the panel, and the
bend keeps the middle: x' = sin(theta)/k has slope 1 and z' = (1 - cos(theta))/k
has slope 0 at x = 0 (`bend()`; checked numerically), so the middle's density is
the flat one at any curvature. (d) What the rule does not cover is the EDGES,
which the bend moves nearer. Edge density over middle density, from `bend()`
under a pinhole at the origin, the panel's middle d half-widths away and the
gain equal to the half-width (the model's d is not any number the instrument
reads):

  curvature   d=1.1   1.5    2.0    3.0    4.0    6.0
  0.1         1.48    1.31   1.21   1.12   1.07   1.03
  0.3         2.72    1.75   1.32   1.01   0.88   0.77
  0.5         3.26    1.28   0.69   0.34   0.23   0.13

So a NEAR bent panel enlarges its edges' texels more than its middle's: at the
fitted width (one texel per eye pixel at the middle) the edges look softer than
the middle, by up to the table's factor; beyond about 3 half-widths they show
less. That is geometry, not a measurement on Sean's rig (his d is unknown), and
it is not corrected: doing so needs d and the bend inside the instrument, which
would be a guess. The flight looks at it (fitted against an explicit 5040).

NOT DONE, said and not hidden. (1) `advanced.vr_camera_census`'s eye-draw hook
still sits in the flat tail, so the census sees no composite while the screen is
curved. (2) Phase 1's take with curvature is unflown: the decision for a
substituted screen is pinned (`ui_layer_world_test`) and the take draws the strip
into the layer as it always did; this build does not change that path. (3) The
intro movie, the splash and the loader's own screens have their own geometry and
are not on this path. (4) The edge softness above.

FLIGHT PLAN (Frontier, HMD Cinema, Sean's pairing: the main menu first, then one
on-foot spot, 60 s a leg, so two 30 s windows). Every leg, `edvr.ini`: `[fix]`
`temporal_aa = dlss`, `ui_quality = 100`, `panel_distance = 0.7`,
`vscreen_res_width = auto`; `[experimental]` `temporal_aa_on_foot_world = auto`,
`on_foot_maps_sharp = off` (one change at a time; leg E turns it on). Install by
`tools\install_edvr.py`; the first line of every read is
`edvr_log.py --expect-build HEAD --route-curve` (and `--vscreen-fit` for the width).
A. `panel_curvature = 0.3`; delete `edvr_logs\vscreen_auto_footprint.txt`. Expect
   `rule=fitted source=seed route=run ... auto = 3504 wide` on a 4032 eye (the
   width flat gives; before this build `rule=legacy route=no`, 5040), the OWNS
   line with the curve sentence, `curve=pending` for at most the first two
   windows and then `curve=0.300/64/<gain>` in every owned window with `curve-reissues`
   equal to `eye-takes` and above 0, one `panel curvature: the VR world route's
   layer drew the same 64-column strip` line, no `fault=` in the `vr world route
   layer:` line's own refusals, and a `vscreen footprint` line with `draws`
   above 0 (before this build `draws=0`: the curved composite was never seen)
   and its `fp=`. Sean's eye, at the menu and on foot: the bend and the
   placement are the route-off screen's, and nothing jumps when the route
   engages (the arrival spell's end) or releases.
B. Same spot, same view, `panel_curvature = 0` (a live edit is enough: the
   curvature is live and `curve=` goes `off`). Expect `curve=off`,
   `curve-reissues=0`, `eye-takes` above 0, a flat screen, and A's `fp=` within
   2% of this leg's (the bend keeps the middle, so the middle's footprint is the
   flat one).
C. A against B in the headset with the same text on the screen: the middle, then
   the edges. The edges softer at 0.3 than at 0 is the table's geometry, not a
   defect; then an explicit `vscreen_res_width = 5040` at 0.3: if the edges
   sharpen and the middle does not change, the fit wants an edge term (record
   both widths and Sean's words). Frame time: the `EDVR GPU census:` and
   `native benchmark:` windows of A against B (a 130-vertex strip for 4:
   expected equal within the windows' own spread; a gap beyond the spread is a
   finding, not a number fixed in advance).
D. Restart with A's ini and the stored footprint kept: `source=measured`, the
   width the footprint gives (roundTo16 of A's measurement; 3504 when it is
   3504 +-8). That is the auto-fit's CALIBRATION with curvature on.
E. (the Phase 1 take with curvature, unflown) `on_foot_maps_sharp = on`,
   `panel_curvature = 0.3`, the route off: open the galaxy map on foot and drag
   it. Expect `screen-takes` above 0 and `screen-draws` equal to it
   (`--maps-sharp`), the map sharp, the screen as curved as before it opened.
PASS: in A every owned window has `curve-reissues` equal to `eye-takes` and
above 0, no `stood-down`, a footprint with `draws` above 0, and Sean sees the
route's screen bent like the route-off screen; B has `curve=off` and the same
`fp=`. FAIL, each with what it means: `eye-takes` above 0 and `curve-reissues=0`
under a `C/S/G` curve (the layer drew a FLAT screen under a curved game draw:
STOP); `curve=stood-down` (a fault; the line before it names it); `curve=pending`
for more than two windows (the SIZE was never read: the game draws flat and the layer
re-issues flat, consistent but flat at 0.3 with the route on); a bend that
changes shape or place when the route engages or releases (the layer's strip is
not the substitution's: compare `curve=`'s C/S/G with the `panel curvature:`
line); `draws=0` at 0.3 (the instrument's new call never ran); A's `fp=` off B's
by more than a few percent (it read something other than the flat quad).
`edvr_log.py --route-curve` says the route's half of this in seven verdicts: CURVE,
RE-ISSUE, READY, STOOD DOWN, STALE BUILD, OWNS and FAULT (exit 1 on a STOP, 3 when
the log has no route line); `--vscreen-fit` reads the footprint and the width.

GATES (all in `build.bat`; the full build is green and the stamp is in the
hand-off). New: `tools\panel_curve_test`, the real `panel_curve.cpp` on WARP:
2271 checks in 11 cases and 85 mutants of the module, every one caught. The
substitution and the layer's re-issue draw one strip, in state, arguments and
bytes, over curvature x columns x sign x gain; a live change rebuilds before
the next re-issue; the game's input assembler comes back whatever it held (a
canonical, an odd and an empty state); a faulting draw stands the feature down.
`tools\vr_world_route_test\mutants.py`: 38 mutants of the route's pure, GPU and
layer rigs, `--self-test` in the build and `--run` on demand. Changed:
`vr_world_route_test` and `vr_world_route_gpu_test` (`curve=`, `curve-reissues=`,
the per-state OWNS suffix, the route running with curvature above 0; in-rig
controls on each pin), `ui_layer_world_test` (`testCurvedScreen`: a substituted draw is
planned and taken like a flat one; a re-issue that did not land closes its
bracket and takes no eye), the `ui_quality_test` wiring pins (the curve branch,
`worldScreenReissueCurved`, `curvedScreenSwallowed`'s gates, the flat re-issue's
exact text, the recognition at three places; 23 in-rig controls),
`vscreen_fit_test` (the rule no longer consults the curve; the footprint's
second call site; 87 of 87 mutants), `on_foot_maps_test` P4a (three places),
and the reader (`edvr_log.py --self-test` builds its logs from the formatter-held
lines of `camera_census_fixture.log`, pins every token it swaps, and every verdict
has a mutant that must fail it).
CURVATURE 0 is held by `panel_curve_test` C1 (nothing wanted; the re-issue is not
ready and draws nothing), the wiring pin on the flat re-issue's text and the
route's 5 s line being the old line with ` curve=off curve-reissues=0` after
`steady-detail=`. CURVATURE ABOVE 0 WITH THE ROUTE OFF is the substitution as it
was: `panelCurveSubstitute(.., !worldReissue.on)` keeps its motion pass (wiring
pin, `panel_curve_test` C2); the one addition is the footprint call, armed only
when the fit is. NOT COVERED by a rig: the draw in the game (no flight yet), and
the strip's index order against the game's culling (flown at 0.3 in August on
the on-foot composite, not re-proved here).

THE CLEANUP (2026-10-01; branch `claude/key-cleanup-defaults`, BUILT, NOT FLOWN).
Sean approved each item after its key and its behaviour were quoted to him.
REMOVED, the behaviour each one chose is now permanent and has no key.
`experimental.temporal_aa_on_foot_world_jitter` (on|off, default on): the route
always jitters the world's cameras while it is Warming or Owned; only the global
`experimental.temporal_aa_jitter` off stops it (`jitter=off` on the 5 s line, the
route then resolves an unjittered world as flight 1 did).
`experimental.temporal_aa_on_foot_world_steady_detail` (on|off, default on; read
by this route and by the flat runtime on foot): the depth-checked steady detail
is always on, in VR and in flat. Both callers hand the resolver `steadyDetail =
true`; the resolver's own field and shader contract stay (the rigs drive it both
ways) and so does the flat 3D menu's blanket `staticScene` policy. The
`steady-detail=on` tokens of the 5 s lines stay, constant, because the reader
parses them and has to keep reading the logs of builds that had the key; the
key-change log lines are gone. Section 84's phase switch
(`experimental.temporal_aa_jitter_follows_upscale`) is removed in the same
commit, with the finding that made it pointless (section 84, end).
DEFAULTS FLIPPED, the keys kept for one release candidate as the way back:
`experimental.temporal_aa_on_foot_world` is auto (was off) and
`experimental.on_foot_maps_sharp` is on (was off; the maps arc, design-world-
camera-motion-2026-09-30.md). Both still need the UI layer live (`fix.ui_quality`
on, a temporal mode on), so an install with `fix.temporal_aa = off`, the shipped
default, keeps the two-eye route; it now logs the route's 5 s lines (zeros) and
one line saying why the route stays off. The vscreen auto-fit's route condition
flipped in the same commit (a width fitted for a route that does not run would be
wrong; config_test and R12b hold the resolver's and the route's fallbacks to the
shipped file).
WHAT AN EXISTING INI SEES (installer_test pins each). The three removed keys were
live lines under [experimental] in every install, so the merge carries each with
its value under "# carried over from your edvr.ini; this version no longer uses
it", reports it as a retired setting (a hand-installed file with no base copy
reports it as a key this version never shipped), and the runtime's config audit
names it in the log as a line this build does not read. An install whose route
and maps lines still say what the previous version shipped (off), with the
installer's base copy kept, moves to auto and on and the report says so. A
hand-installed file with no base copy keeps its off (the merge over-preserves, as
it did for `fix.ui_quality`), and so does a value somebody chose; a deleted line
stays deleted and the code's fallback then answers auto and on.
PINS. `retiredKeyScan` in config_test (the three names appear in no source, the
installer's included, and not in the shipped ini, with its own control); the
key-on rigs became the permanent pins: vr_world_route_test (the decision takes no
key; steady detail unconditional), vr_world_route_gpu_test (the world is jittered
with the route on, the global key off still zeroes it, the refusal line appears
with the census off), flat_hdr_route_tests (both resolver-frame fill sites set
steadyDetail true, no key read); `vr_world_route_test\mutants.py` 46 mutants, 8
new, all caught.

## 83. Flat upscaling: the final copy admitted by structure (build, 2026-10-01)

Branch `claude/flat-upscale`, cut from main `e6cf26b3`. Built, rig- and
mutation-tested; NOT FLOWN. Sean approved the build on 2026-10-01 ("Go ahead
with all 3"; for flat below 1.0: "In flat, running less than 1.0 should be
workable, that's our upscale"): (1) flat R < D upscales whatever bloom, depth
of field and blur do; (2) the stand-down line and the F8 warning say the real
cause, and flat's supersampling advice goes; (3) TAA above the output works or
says why, and the startup false warning goes; (4) VR only: a warning when
Elite's Supersampling is below 1. Readers: `python tools\edvr_log.py --target
<game dir> --flat-upscale --expect-build HEAD` and `--vr-supersampling`.

**Evidence.**

- rc.5 user, build 6ABDAB8E, 2026-10-01 (`edvr_gfx_20261001_145335.log`,
  `_145604.log`; Custom preset AAMode 4, Bloom 3, DoF 2; output 2560x1600).
  Elite's resolution 2560x1440 at supersampling 0.85 renders 2176x1224.
  `flatUniformScale` allows abs(w*oh - h*ow) <= ow+oh, here 348,160 against
  4,160, so 1,574 frames read `invalid-tone-pass` and 4,500
  `no-known-tone-pass` (summed from `flat runtime refusal 5s:`) and F8 said
  only "Elite's post-processing is not recognised". At R > D the HDR route
  treated every frame (367 of 367 in one window at 3264x2040). EDVR's TAA at R
  = 3840x2400 (1.5 D), 14:54:10: the route declines TAA above D
  (`route-does-not-evaluate-at-render-size`), the copy route refused all 308
  frames for `no-known-tone-pass`, the work stood down and F8 asked for
  Anti-aliasing, Bloom and Depth of field off.
- The startup false warning, `_145604.log` 14:56:12.369: 740 frames with a
  final copy and no scene were refused as `no-known-tone-pass`, the work stood
  down after 5 s and F8 named the post chain for 10.5 s, until the first scene.
- Sean 2026-09-30, SS 0.75 (R = 2880x1620 on 3840x2160, AAMode 0, Bloom 3, DoF
  2; section 81): 299 and 149 frames refused, stand-down at 13:32:49, F8
  advising supersampling 1.0, the opposite of an upscale. In every refusal the
  copy was valid and the HDR detector fired on every frame at R >= D.
- The corpus (17 captures, 46 frames; `flat_temporal_test --trace-structure
  <file> [pretend]` prints the structure's verdict per frame): 42 frames have a
  known tone pass, 3 (Bloom 3, DoF 2, R = D) the whitelist refuses, 1 is a 2D
  menu. With the tone pass asked away the structure admits 45 scenes: the 42 to
  exactly the whitelist's selection, and the 3. Of the 45, 24 frames in 8
  captures are below the output (0.5, 0.65, 0.667 and 0.75 D), 12 at R = D, 9
  at 1.5 D. Only the four section 81 captures say the game's AA was off.

**Design.** `src\d3d11\flat_copy_structure.h` (pure; the runtime and the rig
call the same code) holds `flatCopyAdmit`. It runs at the game's final copy,
after the reducer and the HDR detector and before the stand-down merge, and
replaces only a tone-pass refusal (`no-known-tone-pass`, `invalid-tone-pass`).
A frame is admitted when:

1. the copy is the exact copy pair, one draw, full viewport, no depth (the
   reducer stops at the tone pass before it checks the copy, so it is checked
   again);
2. the copy's source S was written this frame by exactly one pass: R8G8B8A8,
   full viewport, no depth, the scene's size, and not written outside a draw
   after it (no Clear, Copy, Update or Map: the prefix model marks the target,
   which is what the whitelist's tone count loses and an unknown tone pass has
   no count to lose);
3. the scene is the R11G11B10F target with a depth of its own size that took
   the most draws (at least 8), and its size is a uniform scale of the output
   from half to twice (`flatRenderFitsOutput`), for a mode that has a route
   there (EDVR's `dlaa` has none below the output: admitting the frame would
   make it Treatable and the resolver would refuse every one in silence, so it
   stays refused as before);
4. the HDR trigger was seen this frame, is unambiguous, reads that target and
   comes before S's first write;
5. no other R-sized R8G8B8A8 pass was written between the trigger and S (the AA
   rule below);
6. the HDR selector (`flatSelectHdrFrame`, new gate `UniformHalfToDouble`)
   finds H's camera, depth and sources in order.

Nothing is asked of the passes between H and S, so bloom, DoF, every tone
variant and a mod's grade stop mattering. The selection is the whitelist's
field for field (S as `color`, the trigger's H or the 3D menu copy's inherited
HDR as `hdr`), so the copy stage, the resolver and the menu policy run
unchanged. It applies only with `experimental.temporal_aa_before_post = auto`,
never where the whitelist selected, never where the HDR route serves the frame
(R >= D, the mode evaluates at R, route not latched). At R < D the game's own
copy upscales S to D and DLSS or FSR resolve S at R to D at the copy (`flat
route: trained-upscale R=2880x1620 E=3840x2160 D=3840x2160`). EDVR's TAA above
D is the display-grid route, also the copy's, so the same rules admit it up to
twice D.

The risk is a game anti-aliasing filter between tone and copy: FXAA would give
a softer double AA, the game's TAA jitters the camera itself and fights EDVR's
phase. The structure cannot tell which, but any such filter adds an R-sized
R8G8B8A8 pass between the scene's first consumer and S. Rule 5 declines that
frame, so it stays refused and F8 keeps its Anti-aliasing advice; the chain
length rides the decline and every window (`ldr-passes-before-max`). No corpus
scene has such a pass, so the rule does not catch known stock, EDHM or bloom
chains, and what AA writes is unobserved until leg 6. Relaxing the rule for
FXAA is one line, after that flight.

**What is named now.** Two reasons are appended to the selector's (23, 24; no
contract hash or published word moves): `render-size-does-not-fit-output` (the
scene is not a uniform scale of the output from half to twice; the measured
sizes ride it) and `no-3d-scene` (a final copy and no scene). Both stand the
work down after 5 s; the first warns, the second never does, so the startup
spell is silent (`hdrFrameEnd` leaves it alone too: the route's no-hdr-consumer
verdict, for a frame with an HDR-shaped target of a few draws, would warn). The
sizes are the prefix model's measurement at every final copy that has a scene,
and the swap chain's, never Elite's settings file.

- Stand-down: `flat stand-down: entered ...: every frame for 5.0 s (598 frames)
  was refused for render-size-does-not-fit-output (Elite renders 2176x1224 on a
  2560x1600 screen), none treated; ...`, and the 30 s reminder.
- F8, render size: "DLSS is not active: Elite renders 2176x1224 on a 2560x1600
  screen." then "Set Elite's resolution to your screen's, 2560x1600, and change
  the render size with its supersampling." (outside half to twice: "That is
  under half the screen's size. Raise Elite's supersampling.", or "over twice
  ... Lower ...").
- F8, any other refusal: "Elite's post-processing is not recognised." and, with
  the key auto, only the Anti-aliasing advice (Bloom and Depth of field no
  longer cause a refusal); with the key off the old advice. EDVR's TAA with the
  scene above the output adds "Above 1.0 supersampling, EDVR's TAA works only
  on a post chain it knows. Set Elite's supersampling to 1.0 or lower, or
  choose DLSS or FSR."
- Gone: the supersampling paragraph and its code (`kFlatSupersamplingWords`,
  `flatHdrSupersamplingAdvice`, `g_hdrBelowOutput`). The warning's log line
  joins its paragraphs with a vertical bar (closing section 81's cosmetic item
  1); the `DLAA` label at SS 0.75 (item 2) goes with the treatment.

**VR only: Elite's Supersampling below 1.** Elite then draws the 3D world into
a target smaller than the eye texture and scales it up before EDVR sees it, so
DLSS upscales an upscaled image and the holograms miss their draw (the
2026-09-24 entry of `docs\openxr-performance-review-2026-09-14.md`: 1998x1931,
75% of a 2665x2575 eye). The detection is vScreen's existing measurement
(`vScreen: the world on this rig is rendered at WxH and scaled into ...`)
against the eye texture, under 98% of both axes
(`src\common\vr_supersample_notice.h`), with vScreen's own guards: the eye's
shape to within a percent, once a session, only while the submitted size never
reached scene levels. It never reads Elite's settings file, never fires in flat
(no eye texture) and speaks once: the log line `vr supersampling: Elite draws
the 3D world at 1998x1931, 75% of the 2665x2575 eye texture, ... Set Elite's
Supersampling to 1 and raise HMD Image Quality instead ...`, a headset toast
("Elite Supersampling is below 1: use HMD Image Quality") through the menu's
own toast queue (`menu.toasts`), the Status page's hint while the menu is open,
and the menu's log line saying which were queued. No page gains a line for it
(a review finding): the menu bitmap is refused above 2048 px and the panel then
keeps its old bitmap, which a note under a settings page's rows (16 lines and a
hint) reaches from a 48-px cap and one more Status line from 54, and the Pimax
has 51-55; the hint is the two-line area the Status page has anyway. An
upscaler in the chain (FSR, NIS) reads the same, and the log line says so. A
pinned `advanced.eye_render_size` is not a measurement and does not trigger it.
It is vScreen's once-a-session measurement: after Supersampling is fixed in
session the hint stays until a restart.

**Contracts: what works today is unchanged.** Pinned in `flat_temporal_test`
(the corpus replay is the test) and `vscreen_fit_test`:

- the reducer, frame contract and whitelist selector are untouched; the
  corpus's 46 contract hashes replay identical;
- all 42 whitelisted frames come back from the admission field for field (key
  auto), and with the key off the structure leaves every frame with a scene
  alone; the 3D menu's `staticScene` is the same through the structure, 42 of
  42;
- the HDR route: the admission answers `route-serves` for DLSS, FSR and TAA at
  R = D and DLSS and FSR at 1.5 D; the 3 Bloom 3 / DoF 2 frames stay the
  route's at R = D; the route's own pins pass except those that held the
  supersampling advice;
- `flatSelectHdrFrame`'s default gate is the route's `R >= D`, held row by row;
  the structure's gate is a uniform scale from half to twice, both ends in;
- with the tone pair renamed, all 24 corpus frames below the output are refused
  by the whitelist and admitted by the structure to the same selection in DLSS,
  FSR and TAA (72 of 72), and none with the key off;
- no new key, no ini value moved, `config_test` unchanged.
  `experimental.temporal_aa_before_post` is the switch: auto is the route plus
  the structure, off the whitelist alone (the A/B); its `edvr.ini` comment says
  so. With it off the whitelist still decides; only the naming changes (a frame
  with no scene says `no-3d-scene`, a render size that does not fit says so),
  and the key's log line says that.

**What the log shows.** If the new code never ran, there is no `flat copy
structure 5s:` line at all.

- Every 5 s while a temporal mode runs, zeros included: `flat copy structure
  5s: key=auto copies=N whitelist=N admitted=N declined=N selector-refused=N
  no-scene=N render-size=N route-serves=N key-off=N last=<outcome> scene=WxH
  output=WxH source=WxH VS=.. PS=.. ldr-passes-before-max=N
  declines=<cause:count,...>` (or `none`). Pass at R < D, Bloom 3 and DoF 2:
  `admitted` about `copies`, `whitelist=0`, `declines=none`. A known chain
  (Bloom 0, DoF 0): `whitelist` about `copies`, `admitted=0`. Fail: `admitted=0
  declined=N` with a cause, or `render-size=N`.
- Once: `flat copy structure: first admission at frame=N ...: ... admitted by
  structure`. At most 12 a session, each cause once: `flat copy structure:
  declined at frame=N: <cause> (the whitelist said ...)`. The causes are the
  decline strings in `flat_copy_structure.h`; the AA rule's is
  `r-sized-image-passes-follow-the-first-consumer-of-the-scene-hdr`.
- `flat stand-down: ... refused for no-3d-scene` at startup, silently, then
  `resumed` at the first scene; `render-size-does-not-fit-output (Elite renders
  WxH on a WxH screen)` for a mismatched resolution; `flat settings warning:
  shown (mode=DLSS, frames refused for render-size-does-not-fit-output, work
  stood down, structure admission on, render WxH on output WxH): ...`. No
  `Supersampling is below 1.0` anywhere.
- VR: `vr supersampling: Elite draws the 3D world at ...` after vScreen's
  adoption line, then the menu's `... queued as a toast` line.
- The reader's tags: KEY, ADMISSION, TREATED, UPSCALE, TONE REFUSALS,
  STAND-DOWN, F8 WARNING, CHAIN, ADVICE (a STOP when the old advice is in the
  log, i.e. the wrong build). `--vr-supersampling`: NOTICE, CONSISTENT,
  HEADSET, FLAT. `tools\flat_upscale_fixture.log` (a good flight and three
  episodes) is held to the formatters by the rig and read by the reader's
  self-test.

**Environment.** Flat profile, Windows, Elite's D3D11 renderer, no VR runtime
(N/A), mono SDR output, DLSS through NGX (model k on Sean's rig), FSR3 or
EDVR's TAA. R is the prefix model's R11G11B10F scene target; fixed tables: 128
targets, the scene's 8-draw floor, 12 decline lines a session.

**Flight plan** (Epic install, flat build, `edvr-flat.ini`; read each log with
`--flat-upscale --expect-build HEAD` first, a log from another build is no
evidence). Elite's resolution is the screen's own, borderless, AAMode 0 unless
a leg says otherwise; the `flat settings:` line records AAMode, Bloom and DoF.
Ini `[fix] temporal_aa = dlss` (`fsr`; `on` is EDVR's TAA) and `[experimental]
temporal_aa_before_post = auto` (the default).

1. SS 0.75, Bloom 3 and DoF 2, DLSS. Pass: the first-admission line, route
   `trained-upscale R=0.75 D`, `admitted` about every frame, `whitelist=0`,
   `declines=none`, nothing refused for a tone pass after startup, no
   stand-down but the silent startup `no-3d-scene`, F8 hidden, reader PASS.
   Look at: the picture against the game's own upscale at the same SS, bloom
   and DoF edges, the HUD and holograms.
2. SS 0.75, Bloom 0 and DoF 0 (a chain the whitelist knows). Pass: `whitelist`
   about every frame, `admitted=0`, treated: the structure leaves known chains
   alone.
3. Legs 1 and 2 at SS 0.85.
4. A mismatched resolution (an aspect other than the screen's, such as
   2560x1440 on a 2560x1600 screen at SS 0.85, or 16:9 on a 16:10 screen),
   DLSS. Pass: after 5 s the stand-down names `render-size-does-not-fit-output
   (Elite renders ... on a ... screen)`, F8 shows the two sentences above and
   nothing about the post chain; reader WARN (STAND-DOWN, F8 WARNING), no STOP.
   Then set the resolution to the screen's: `resumed`, warning `hidden`.
5. `temporal_aa = on` at SS 1.25, Bloom 3 and DoF 2, AAMode 0. Pass is either
   outcome, named: treated through the structure (`taa-display-grid-down`,
   `admitted` about every frame), or a decline cause in `declines=` with the F8
   TAA paragraph (`the-copy-source-is-not-the-scene-size` would say Elite
   downsamples before the copy above SS 1). Fail: a bare "not recognised" with
   Bloom and DoF advice.
6. Game AA on (AAMode 4 as the rc.5 user, also 1 to 3) at SS 0.75, Bloom 3,
   DLSS, then `on` at SS 1.5 (the 308-frame case). Expected by design:
   `r-sized-image-passes-follow-...` declines with the chain length, stand-down
   for `no-known-tone-pass`, F8 "Turn off in Elite's graphics options:
   Anti-aliasing" and nothing about Bloom, reader WARN (CHAIN). Read the chain
   length and which filter it is, then decide whether the rule relaxes.
7. `temporal_aa_before_post = off` (a live ini edit) at SS 0.75, Bloom 3 and
   DoF 2. Pass: `key=off`, `key-off=N` in the windows, the old refusal and old
   advice. The A/B for leg 1.
8. VR at Elite Supersampling 0.85, HMD Image Quality 1.0, `[menu] toasts = on`.
   Pass: `--vr-supersampling` NOTICE with about 85% of the eye, CONSISTENT,
   HEADSET; in the headset one toast and, with the menu on the Status page, the
   hint (open every page too: none may stop drawing). Control: Supersampling
   1.0 gives no `vr supersampling:` line, and a flat log never has one.

**Mutations** (scratch runs, restored after): `flat_temporal_test` 51 edits, 50
applied, 49 caught by the rigs and 1 equivalent (the `selected()` term of the
entry test is redundant with the reason test); `vscreen_fit_test` case R13, 16
mutations in `mutants.py`.

**Review.** A read-only review of the diff by a second agent found one defect
that no rig could see (the VR note's layout, above) and these, all fixed: the
admission now declines an S that was written outside a draw and a mode with no
route at the size; `hdrFrameEnd` no longer turns no-3d-scene into a warning;
the scene's sizes are refreshed at every final copy; the key-off log line names
what it still names; and the readers (an exact two-axis compare for the 98%,
frames counted from the windows and not the decline lines, ADMISSION a WARN
when no window ruled on a copy, no claim from silence about Elite's
Supersampling).

**Not built.** The relaxed rule for a game FXAA chain (waits for leg 6); a
trace marker for the structure's verdict (a live F10 trace replays as the
whitelist's refusal, and the rig runs the admission over it); a VR notice for a
pinned `advanced.eye_render_size`; a guard for a compute write into S or H of
an unknown chain (the whitelist's tone count guards them for a known one; the
prefix model records no compute write for other targets); a stand-down message
for `dlaa` below the output (it stays refused with the old words).

- ruled out: the final copy or the HDR detector as the cause of the refusals,
  because in every refusal the copy was valid and the detector fired on every
  frame at R >= D.
- ruled out: growing the whitelist hash by hash, because the refused chains
  differ by user and setting and the structure admits all 42 known frames and
  the 3 unknown ones with no list.
- ruled out: relaxing `flatUniformScale` for a mismatched resolution, because
  2176x1224 on 2560x1600 is 348,160 off against 4,160 and the game's own copy
  would stretch the image unevenly: the user must set the resolution, and F8
  says so.
- ruled out: the stopgap (the HDR route as DLAA at R, the game upscaling) as
  the first answer, because structure admission proved feasible on the corpus;
  nothing was switched silently.
- ruled out: the flat supersampling advice as the answer below 1.0, because
  below 1.0 is served now and the advice told the user to raise supersampling.
- ruled out: admitting a chain with an R-sized image pass after the tone pass
  before a flight reads one, because FXAA would be softer, the game's TAA
  jitters the camera itself and the structure cannot tell them apart.
- ruled out: a new ini key for the admission, because the route's key already
  means off = whitelist alone, auto = route plus structure.
- ruled out: Elite's settings file for the VR Supersampling warning or the
  render size in the F8 words, because the measured sizes are the evidence and
  the file says only what was asked for.
- ruled out: a note on every settings page and a Status line for the VR notice,
  because the menu bitmap's 2048-px height guard trips from a 48-px cap with
  the note and a 54-px cap with one more Status line, and the Pimax runs 51-55:
  the panel would keep a stale bitmap on the headsets that run Supersampling
  below 1. The toast, the log line and the Status hint add no line.

F8 WARNING DEBOUNCE (2026-10-01; claude/key-cleanup-defaults, BUILT, NOT FLOWN).
Sean's flat flight on the Epic install (`edvr_gfx_20261001_103559.log`, build
6ABE8A79) showed the render-size warning flipping five times in nine seconds,
10:38:16.516 to 10:38:25.594, between "Elite renders 1440x810 on a 3840x2160
screen" and a transient 256x256, a loading screen's target. The scene's size moves
only when a final copy is evaluated: every frame while the work is treated, but
only on the stand-down's probe frames (every 1500 ms) while it is stood down, so
each transient was the computed cause for exactly one probe interval (1.509 s and
1.521 s). No real state that carries a message lasted under 3.0 s in that session
(1440x810 shown for 3.01, 3.04 and 10.58 s; the show already waits 5 s).
THE RULE (`flat_elite_settings.h` `FlatWarnHold`, asked by `menu.cpp`
`flatWarningTick` once a tick, before its own comparison): while a warning is on
show and the runtime is still refusing, a different computed warning key is adopted
only after it has been the computed key on every tick for 2000 ms
(`kFlatWarnHoldMs`, two agreeing probes: 0.48 s above the transients, 1.0 s below
the shortest real shown state). A key that comes back to the shown one drops the
change; a third key restarts the clock; show and hide stay immediate. Replayed at
the log's stamps the rig gives zero adoptions and the shown key never leaves
1440x810 (tick steps of 1, 7, 16 and 33 ms); controls: a hold of 0 reproduces the
log's four changes, 1500 lets both transients through, 1999 ms holds and 2000
adopts; 19 mutants of the header and menu.cpp, each caught. The log: a bounded
line (8 a session, beside the 24 of the shown, changed and hidden lines) when a
change starts being held, "flat settings warning: a change of cause is held for
2000 ms before it replaces the one on show (on show: ...; computed now: ...)"; it
starts with neither shown, changed nor hidden, so the `--flat-upscale` reader counts
no warning from it. If the hold never ran, a flight like 103559 shows one `shown`
and four `changed` lines in nine seconds and no `held` line; with it, two `held`
lines and no `changed`. A flight with no transient shows neither.
ruled out: a loading-screen size filter, because 256x256 is square on a 16:9 output
and every other size seen is exactly 16:9, so the aspect cannot tell a transient from a
mis-shaped real resolution, and a size floor alone would make those probes "no
scene", which HIDES the warning: a different flicker.
ruled out: a hold of 1.5 s or less, because the transients last one probe interval, so
1.5 s passes both (a 10-20 ms margin) and 1.0 s passes half of one.
Residual, recorded: two consecutive transient probes would pass; a no-scene probe
still hides and re-shows; a change the user makes (the mode, an Elite setting) also
waits 2 s before the words change. Neither of the first two was seen.

## 84. Jitter phases follow the upscale ratio: a switch to fly (build, 2026-10-01)

Suspect (g) of section 82. Sean's VR cockpit, landed: an eye render of
2016x1949 into an output of 4032x3898 (2x), and distant hills keep
shimmering after DLSS. History is kept and motion is correct; HMD Image
Quality 0.65 helped but did not fix it. Every path here jitters through the
same eight Halton (2,3) phases (src\common\temporal_math.h,
`kTemporalJitterCount` 8), which repeat at 11.25 Hz at 90 Hz. NVIDIA's DLSS
guidance and AMD's FSR (`ffxFsr3UpscalerGetJitterPhaseCount`, which casts
8 x (display / render)^2 to an integer; docs/fsr-upscaler-design-2026-09-16.md)
both ask for 8 x (display / render)^2 phases, 32 at 2x, so that the upscaler
sees enough sub-pixel samples of fine detail. Eight is that rule at 1x only. A hypothesis,
not a finding: (g) also names NGX's integration of fine HDR content, and the
same shimmer in all three backends would put it upstream of the count.

What was built, commit 2 of claude/jitter-phases-terrain-retire (commit 1 is
the terrain retirement, docs/terrain-motion-dispatch-cost-2026-09-17.md):

- The rule. `temporalJitterPhaseCount(renderW, renderH, outW, outH)` is
  ceil(8 x output area / render area): the ratio's square taken as the ratio
  of the two areas, which is the square of the per-axis ratio whenever the
  axes scale alike (every size EDVR has met), as an exact integer ceiling
  with no float rounding; never below eight; capped at 128 (NVIDIA's own
  3x figure is 72). `temporalJitterPhase(n, count)` is the same Halton (2,3)
  walk with the index (n % count) + 1, so its first eight are the fixed
  eight's; `temporalJitter(n)` is exactly count 8, bit for bit.
- The switch. `experimental.temporal_aa_jitter_follows_upscale`, off | on,
  default OFF: every path then runs today's eight, byte for byte (pinned).
  Live: the VR pass reads it at each treat, the flat route at each Present.
  A change resets nothing (the key is no part of any history). TEMPORARY:
  Sean drops it after the flight if it wins. The in-headset menu's developer
  mode lists it on the Experimental page (a getBool read is a toggle).
- The VR eye pass (native_temporal.cpp). Each eye's last treated input and
  the output the pass was asked for, the served floor's cut included; the
  larger of the two eyes' counts, so both eyes keep sharing one phase
  (docs/fss-scanner.md relies on it). begin() runs before the treat that
  sizes the frame, so a change takes one frame, two after a mode change, and a
  first treat must have named a size (eight until then).
- The flat route (flat_runtime.cpp, `flatCameraPhaseCount`): R is the plan's
  render size and E the size the upscaler resolves to (the vendor's negotiated
  size where one was answered for the contract, else the route's default: D
  for DLSS, FSR and TAA below the output, R at or above it), on the UPSTREAM
  camera route only. The Legacy route's lighting patch
  (flat_lighting_contract.h) refuses a jitter past 7/16 of a pixel; the first
  eight phases stay inside it, the ninth (y -0.463) and the sixteenth (x
  -0.469) do not (pinned in flat_lighting_tests.h), so a Legacy or Off frame at
  a longer count would lose its jitter to a refusal. Only the injector's rows
  carry a phase to the game with no such bound.
- The VR world route (vr_world_route.cpp) resolves at the game's render size
  (render == output), so the rule gives eight with the key on or off: it
  reads no key, hands its phase machine the rule's count for its own sizes
  and prints what the machine ran.
- Untouched on purpose: the FSR3 port's own count (int(8 x ratio^2), ramped
  1 a frame, used for lock lifetime only, ffx_fsr3upscaler.cpp:1004; EDVR
  never asks it for jitter) and NGX (jitter per frame, no count).

The log. VR: `native temporal: jitter phases=N (eye 0 WxH -> WxH, eye 1 ...;
experimental.temporal_aa_jitter_follows_upscale=on|off: ...)` at the first
jittered frame and at every change of the count or the key (a mode change
shows its settling frame too), and `jitter_phases=N` in the close totals.
Flat: `phases=N` on the 5 s `flat jitter:` line. World route: `phases=N` just
before `jitter=` on its 5 s line. One read for all three:
`python tools\edvr_log.py --target frontier --expect-build HEAD --grep "phases="`.

Rigs and mutants. `temporal_test` (the rule's table: 32 at 2x, 18 at 1.5x, 72
at 3x, 19 for the Pimax pair; the ceiling exact; the floor of eight and the
cap; the fixed eight bit for bit), `flat_temporal_test` (the phase machine's
count, a change of count between frames, `flatCameraPhaseCount`, and the
Legacy bound that is the reason), `vr_world_route_test` (the window's token and
a source pin on the route), `native_temporal_test --phase-count-self-test`
(the real channel code flown on WARP through eight segments of mode, size and
live key flips: the sequence frame by frame, both eyes' phase, no reset on a
flip, the logged sequence), `config_test` (the shipped default and both code
fallbacks, each with a control), the VR camera census fixture
(`tools\camera_census_fixture.log`, regenerated: the route's 5 s line gained
`phases=8`), and `tools\temporal_test\jitter_phase_mutants.py`:
20 one-token breaks, each caught by a check that names it (`--run` on demand,
about 30 s; `--self-test` is in the build). All 20 caught.

ruled out: a longer count on the flat Legacy route, because its lighting
patch refuses phases past 7/16 of a pixel (above); a count for the world
route, because it resolves at R = D. Not ruled out: that more phases is not
the lever; the flight below measures the count alone.

The flight (one session with the terrain retirement's regression flight;
verify the build with `python tools\edvr_log.py --target <t> --expect-build
HEAD`):

1. VR, cockpit, landed at a hills spot, HMD Image Quality 0.5, DLSS. Key OFF:
   the log reads `native temporal: jitter phases=8 ...=off`. Take an eye dump
   (the `dump_eyes` hotkey or the menu's Instruments row; with
   `advanced.eye_run_treated` on for the treated crops). Flip the key ON live
   (the menu's developer mode, Experimental page; or the ini): the log gets
   `jitter phases=32 (eye 0 2016x1949 -> 4032x3898 ...=on` at once if the pair
   is Sean's, and the real sizes otherwise. Take a second dump from the same
   pose. Compare the treated crops' per-pixel variance over the run on a hills
   region: `python tools\eye_run_shimmer.py <crops> --centre X Y --region hills
   X0 Y0 X1 Y1`, its last line. PASS: the on run's amplitude is lower than the
   off run's, with no flash in the `whole` column and no blur or ghosting
   added. FAIL: no change (the count is not the lever: open (g)'s other legs)
   or a worse picture (a longer sequence settles slower after any reset).
2. One low flight over terrain with the key on: the terrain retirement's
   regression check (docs/terrain-motion-dispatch-cost-2026-09-17.md).
3. Flat (Epic), DLSS or TAA, the key on, at SS 1.0 and then below it (below
   1.0 the frame reaches the resolver through section 83's admission of the
   game's final copy by structure, which this build carries). The
   `flat jitter:` line reads `phases=8` at SS 1.0 (render at the output) and
   ceil(8 x (D/R)^2) below: 12 at 0.85, 15 at 0.75, 32 at 0.5, with
   `state=live` and `refusals` not rising, and `flat route:` naming the same R
   and E. PASS: that, and the picture at least as stable as with the key off.
   FAIL: refusals rising or the state leaving `live` with the key on; phases
   stuck at 8 below 1.0 (the route is not upstream: the
   `flat camera inject owner:` line says which route owns the frame); and
   watch the lighting for seams on the game's 120-pixel light-grid tiles:
   the upstream route has flown only the eight phases inside 7/16 of a pixel
   (section 13 shows a lookup of p - j stays in its pixel, hence its tile,
   for any |j| < 1/2 less a float margin, which 128 phases leave at 1/256),
   and this is the first time it carries a phase out to 0.496.

FLIGHT, 2026-10-01 11:18 (Frontier 08b48036, log 111827; VR cockpit,
landed, at the distant hills, eye 2016x1949 -> 4032x3898). The switch went
live at 11:20:57.680. The log shows `jitter phases=8 ... =off: the fixed 8`
at engagement, then `jitter phases=32 ... =on` at 11:20:58. Sean flipped it
while looking at the hills: "didn't see any change to shimmering on the
hills". He took an eye dump with it on, where the shimmer was strongest
(stamp 112137); there is no same-spot key-off dump to compare it with.
ruled out: too few jitter phases (suspect (g)) as the cause of the hills'
shimmer, because 32 phases at the same spot made no visible difference.
What remains is the hills investigation's (d): sub-pixel streaks at the 0.5
input resolution, about 21.6 input px per degree. HMD Image Quality 0.65
helped, and the flat game at about 32 px per degree does not shimmer. Proposed: the
switch goes in the cleanup build and the fixed 8 stays, since there is no
visible benefit and the flat light-grid risk above is unflown.

ROOT CAUSE, 2026-10-01 (ruled in): Elite's own terrain checkerboard rendering.
The game option is `TerrainCheckerboardRenderingEnabled` in the active graphics
preset's fxcfg (Sean's `Custom.4.4.fxcfg`, line 4). With it on, distant terrain
reaches EDVR with half its horizontal samples: neighbours in the raw input pair
up beyond about 20 m. There is no pairing in y, in the cockpit or in the sky.
DLSS cannot steady an input that is paired like that, so the distant hills
shimmer. Sean turned the option off in the game and the shimmer went: "That fixed
it!". His flat game did not shimmer with the same settings folder, which is why
the hint that follows is VR only. The option's default per preset, read from the
Epic install's `OptionDefaults\*.fxcfg`: true in Low, Mid, VRLow, VRMedium,
VRHigh and VRUltra, false in High and Ultra, so a VR player on any stock VR preset
has it on until they change it. Settings.xml carries a tag of the same name that
reads true on every machine seen, Sean's included, and is not the toggle: the
game's options screen fills its UI key from the active preset's file (read from
the exe's disassembly, 4.4.1.1; no consumer of the Settings.xml tag was found).
ruled in: Elite's terrain checkerboard rendering as the cause of the VR distant
hills' shimmer, because turning it off removed the shimmer with nothing else
changed.
ruled out (stays): too few jitter phases as the cause (suspect (g)), because 32
phases at the same spot made no visible difference (above).

THE SWITCH IS REMOVED (claude/key-cleanup-defaults, 2026-10-01).
`experimental.temporal_aa_jitter_follows_upscale` and the ratio-based phase path
are gone: the source of 2630bdfd reverted (`temporalJitterPhaseCount`,
`temporalJitterPhase(n, count)`, `flatCameraPhaseCount`, the phase machine's
count, the VR eye pass's count and its log lines, the `phases=` tokens), and
`jitter_phase_mutants.py` with its `build.bat` lines. The fixed eight Halton
(2,3) phases stay, byte-identical to the switch off, and no log carries a phase
count now (nothing in `tools\edvr_log.py` read one). It flew with no visible
change, and the shimmer it was built for was the checkerboard. The unflown flat
light-grid risk above (a phase out to 0.496 under the 120-pixel tiles) goes with
it. An existing ini that carries the line keeps it as a retired setting ("carried
over from your edvr.ini; this version no longer uses it", pinned in
`installer_test`).

THE VR HINT (2026-10-01; claude/key-cleanup-defaults, BUILT, NOT FLOWN). With the
option on in VR, EDVR now says so: a headset toast once per raise of the option, the
Status page's hint, and one log line. VR only (the flat game did not shimmer), no new
key. WHAT IS READ: the toggle is the ACTIVE PRESET's file, never Settings.xml's tag of
the same name. Settings.xml is asked for `<PresetName>` and nothing else. Custom: the
highest `Custom.<major>.<minor>.fxcfg` in `Options\Graphics`, only names of exactly
that shape (the lookalikes `-Custom.4.0.fxcfg`, `Custom.4.0.fxcfg-backup`,
`Custom.4.4.fxcfg.baseline-bak-...` and `TomCatT.4.0.fxcfg0` are never read). A stock
preset: `<game folder>\OptionDefaults\<Preset>.fxcfg` (the exe's folder), where Low,
Mid and every VR preset (VRLow, VRMedium, VRHigh, VRUltra) have it ON and High and
Ultra OFF: a VR player on a stock VR preset has the shimmer until they change it.
Unknown, with the reason in words and no notice: no folder, no Settings.xml, no
PresetName, no Custom file, an unreadable file, a user-made preset, an absent tag, an
odd value. The reader calls `flat_elite_settings.h`'s helpers and copies none.
WHERE IT RUNS: a detached worker thread polls every 3 s (the render thread loads one
atomic word holding the state and a version) with the journal worker's lifetime
(exception net, stop flag, wake event, never-freed session, the pinned module). The
game rewrites Settings.xml and then the preset in place at Apply, so a read can catch
a half-written file: the worker reads again every poll and leaves a known state for
Unknown only when two reads in a row say Unknown. It starts from the first VR frame
boundary (the menu's tick); the flat profile never starts it. THE NOTICE (the VR
supersampling notice's path): the toast, once per RAISE (Off and then On says it
again; the latch is set before the `menu.toasts` test), "Distant terrain shimmers:
turn off terrain checkerboard" (55 characters); the Status hint, which REPLACES the
existing hint text and adds no line, "Turn off terrain checkerboard rendering in
Elite's graphics options." (68 of the 78); the log sentence "Elite's terrain
checkerboard rendering makes distant terrain shimmer with DLSS. Turn it off in
Elite's graphics options." The hint is a live predicate, so it ends when the option
does. One hint slot, two possible hints: the one that applies shows, and when the
supersampling notice applies too they alternate every 6 s. The words say "terrain
checkerboard rendering": no game file names the option's label (the UI strings are in
compressed assets; the exe has the phrase only as a GPU profiler pass name), so the
real label is not confirmed. The hint shows on any VR run with the option on:
nothing checks the selected temporal mode.
THE LOG: `vr terrain checkerboard:` lines, the worker's start, the first read and each
change (16 at most, then a limit line). No such line at all means the worker never
started (a flat session, an older build, or no VR menu tick); "read, off" is a line
too. `python tools\edvr_log.py --target frontier --expect-build HEAD
--terrain-checkerboard` reads them (exit 1 with no line).
GATES. `tools\terrain_checkerboard_test`: 188 checks over 6 cases (fixtures written to
a temp directory: Custom true and false, the highest version winning with the
lookalikes ignored, preset Custom with no Custom file, a missing Settings.xml or
folder, stock VR and non-VR presets, a stock preset with no file, an absent tag, value
spellings, CRLF, BOM, indentation, Settings.xml's own tag not changing the answer,
rewriting the file flipping the state and re-arming the toast, the worker on a real
log, the texts' lengths, the alternation) and 102 mutants, all caught; the `menu.cpp`
wiring is held by source pins. On Sean's real files the reader says OFF (Custom,
`Custom.4.4.fxcfg`, false); on the Epic install's OptionDefaults Low, Mid and VR* say
ON and High and Ultra OFF.
ruled out: Settings.xml's own `TerrainCheckerboardRenderingEnabled` as the toggle,
because it reads true on every machine seen (Sean's, with the option off, included)
and the options screen fills its UI key from the preset's file.
ruled out: reading the files on the render thread; the worker is the only reader.
ruled out: a note on every settings page or a Status line, because of the menu bitmap's
2048-px height guard (section 83); the hint replaces text and adds no line.
ruled out: guessing the game's default for an absent tag, because the default (true
for a fresh preset struct) is inferred from the exe, never seen in a file, and an
inference raises no notice. Section 83's "Elite's settings file for the VR
Supersampling warning" is not contradicted: that was about a fact the runtime
measures (the sizes); this option's state can be read only from the file.
NOT KNOWN: the option's label in the game's menu; the game's own version, so the
highest Custom file stands in for the exact `Custom.<major>.<minor>` match.
FLIGHT (Frontier, VR, the on-foot or cockpit view): with the option ON in the active
preset (a stock VR preset, or the Custom file with true) expect `vr terrain
checkerboard: ... ON`, one toast in the headset and the hint on the Status page; turn
the option off in Elite's graphics options and Apply: within about 6 s the log says
OFF and the hint is gone; turn it on again and the toast comes again. Read with the
command above; the distant hills' shimmer is Sean's eye.

## 85. RX 9070 XT: FSR works in the menu, remains stood down in game (2026-10-02)

Evidence: `edvr-logs-20261002-190903.zip`, graphics log
`edvr_gfx_20261002_190718.log`. Flat profile, RX 9070 XT, FSR 3.1.2,
3440x1440 render and output, SS 1.0, game AA/bloom/DoF off. No VR runtime
loaded. Version v0.18.0, PE link stamp 6ABED11D (2026-10-01 21:31:09 UTC).
`edvr_log.py --expect-build HEAD --version` accepts the release tag as a
substring of HEAD's description; that does not prove exact HEAD. The tag's
flat runtime, camera, HDR and copy-selection code matches this checkout.

Timeline from `edvr_log.py --flat-upscale` and targeted `--grep`:

- Startup/loading: no-3d-scene; stand-down entered 19:07:25.464.
- 19:07:38.982: a probe selected and resumed. FSR initialized and made its
  context. The next two windows treated 975 and 296 HDR frames (1271 total).
  Only two engine-source-not-ready and one incomplete-jitter-frame refusals.
- Loading/transition: stand-down entered 19:07:50.261 after 6666 frames over
  5 s refused for no-3d-scene. This reason remains the entry reason.
- From 19:08:03.776, every HDR probe finds a 3440x1440 target and consumer,
  but selects conflicting-hdr-target-or-camera; admitted/backend/treated
  stay zero. HDR image continuation refused rises to 35, accepted stays 0.
  At 19:08:50.261, the last probe explicitly names the HDR conflict.
- F8 at 19:08:50.858 says no-known-tone-pass. That copy/discovery warning
  does not establish the HDR conflict's cause. No manual F10 audit occurred:
  details=not-manually-armed, audit-pairs=0, bytecode-stages-saved=0.

ruled out: FSR backend initialization or unsupported AMD hardware as the
cause of this stand-down, because the same session resolves 1271 HDR frames.
ruled out: supersampling below 1 as this report's cause, because R = E = D.

Open hypotheses: a Probe recovery artifact, or a real viewport/depth/write/
camera mismatch in the gameplay HDR image continuation. `flat_runtime.cpp`
applyWork pauses camera injection and projection shadows in Probe, but
flatRuntimeMap/Unmap/Update still capture runtime camera rows. Thus zero
camera-refresh calls does not prove missing camera data. `flat_camera_table.h`
newFrame invalidates rows until fresh writes; `flat_runtime_model.h` imageCopy
requires current matching source/HDR provenance. Its rejection sets
ImageCopySource/hdrBad, which `flat_hdr_route.h` flatSelectHdrPrefix returns
as ConflictingHdr. The bundle does not identify which prerequisite failed.
Late passive discovery also drops contract/edge observations; do not treat
its truncation as the earlier conflict's explanation.

Instrumentation limit: reportConflict runs on copy treatment, not at the
HDR trigger; Probe returns before that reporting path. Thus no detailed
conflict witness in this bundle is not evidence of no conflict.

Next flight, unchanged settings/build: in the cockpit while F8 reports FSR
inactive, close F8, press F10 and note whether FSR resumes. After about 2 s,
press F10 again, wait at least 5 s, then bundle logs with the installer.
F10 ends stand-down, restores Full work, arms the 900-frame audit and dumps
the trace ring; the installer includes traces/shaders. The second arm keeps
a trace of Full work as well as the first arm's prior probes. If Full treats
while Probe did not, investigate probe camera provenance/recovery. If Full
still refuses, replay the traces for the first conflicting draw and its
depth/viewport/camera provenance before changing admission. Verify the
version again and check both trace dumps have frames/events and no SHORT
WRITE. No rendering code, config, or installed files changed for this review.

### Second capture: Full work also refuses the source (2026-10-02)

`edvr-logs-20261002-202354.zip`, graphics log
`edvr_gfx_20261002_202027.log`: same v0.18.0 / 6ABED11D binary. FSR selected
at 20:22:10.241. Multiple F10 arms restore Full and complete 900-frame
audits; treated remains 0. At 20:22:59.078, image-copy-source conflicts
number 570 in 5 s. Copy provenance completes two samples per arm, with no
missing source/destination records or actual-shader mismatch.

ruled out: Probe-only recovery failure as the sole cause, because Full
work after repeated F10 arms still treats zero frames with the same conflict.

The failed prerequisite is now identified. At 20:22:55.384 (frame 60522),
the format-9 source has exactly one write: VS CFA91824129ECBBC / PS
07B3F82100F29401, scene-sized depth/DSV, b1=null and camera-present=0.
Its image-source-bad=1, hdr-bad=1, bad-cause=image-copy-source. The HDR
destination is otherwise unmarked before the copy; cached/actual copy
shader hashes match. Frame 60612 repeats the same evidence. The conflict
witness has an empty reference and this source draw as current, rather
than two disagreeing cameras or depths.

`flat_runtime_model.h` exempts only PS FCFAD73924BF45B9 on this VS (and
only after bridge verification). Therefore this source fails the first
format-9 write's cameraCurrent requirement, and the later HDR copy fails
the source bad-state checks. The log classifies the pair generic-inert,
but that proves the VS has no CB/projection; it is not proof the PS reads
no camera. `ambient-occlusion.md`'s earlier disassembly identifies this PS
as a depth-aware upsample of half-resolution colour. Inspect exact bytes
before extending the camera-independent exemption; retain depth/DSV,
extent, viewport and current-frame source checks.

Existing evidence to retrieve, no new flight: the log records successful
trace dumps, including traces/flat_trace_60521.bin (3 frames, 7277 events,
3667768 bytes, no SHORT WRITE). None of the traces or shaders are in this
ZIP. The source bundler accepts same-session files within 180 s and below
64 MiB/file, so this trace is below its size cap; the installed bundler's
version/path/timestamps are unverified. Obtain the existing edvr_logs/traces
files and ps_07B3F82100F29401.dxbc if present under edvr_logs/shaders. Replay
the Full trace and inspect the pixel shader before changing admission.

### Trace replay: the exact source exemption is sufficient (2026-10-02)

`flat_trace_all.zip` provides flat_trace_55758, _56705, _58750, _59615 and
_60521.bin: 3 complete frames each, 15 total, matching the logged F10 dumps.
The current unchanged flat_temporal_test rig was compiled with build.bat's
recipe. All 15 --trace-check results match the live contract hashes;
--trace-chain finds one 3440x1440 HDR candidate, an unambiguous known
consumer and zero late writes, refused for conflicting-hdr-target-or-camera.
The relevant source/model is unchanged from the supporter's v0.18.0.

Focused replay confirms the first bad copied source in every frame: the
single scene-sized format-9 CFA91824129ECBBC/07B3F82100F29401 draw, same
depth/DSV, full viewport, no camera and no image-source-verified flag. The
copy is rejected 0 accepted / 1 refused. Three other format-9 writes per
frame are half-sized with no DSV; they are not the copied source.

An offline counterfactual changes only this source's PS identity to the
already admitted FCFAD73924BF45B9 and sets the verified-source flag. All
15 frames then accept the HDR copy (1 accepted / 0 refused) and select
observed-mono-input-candidate. Motion-source aggregation is rechecked below.
Setting the
verified bit alone does not help: exact-pair admission is required too.
This proves sufficiency at selection, conditional on the actual PS being
camera-independent and passing the live verifier. It does not test FSR
dispatch or raster-phase qualification, and does not authorize a blind
exemption. No production source or installed files changed.

ruled out: ambiguous HDR ownership or late HDR writes as the captured
failure, because all 15 chain replays have one candidate, ambiguity=0 and
late-writes=0. The exact source camera requirement is the admission blocker.

The archive contains no shader bytes. A local historical VS capture hashes
to CFA91824129ECBBC and disassembles to position/UV forwarding, no CB reads;
the exact PS is absent from the project trees and local Epic shader folder.
The older AO journal's summary is not a substitute for its disassembly.
Next evidence: ps_07B3F82100F29401.dxbc from the supporter's shader folder if
it exists. Current F10's named stage probes omit this PS and generic-inert
classification does not trigger unknown-pair saving, so another unchanged
F10 run cannot be relied on to supply it. If absent, add a bounded named
creation-cache capture for this PS before scheduling the next flight.

### Supplied pixel shaders and targeted capture (2026-10-02)

`ps_all.zip`: 40 DXBC files, 141116 bytes. Every filename matches the
actual EDVR FNV64 hash; no file, including a mislabeled one, hashes to
07B3F82100F29401. `ps_1F64463B15189104.zip` contains the same verified
1F64463B15189104 file included in ps_all. It is the separate documented
CE24A73943632F55/1F64463B15189104 scene pair: depth/G-buffer inputs t0..t3,
discard and two render-target outputs, CB2[46].x and CB2[0].x. It does not
qualify the missing image-source pass. The HDR copy PS DFCBA0EC70B03C9B
is present; the existing exempted source PS FCFAD73924BF45B9 is absent.

Diagnostic change: the existing bounded F10 arm in flatRuntimePresent
requests PS 07B3F82100F29401 through captureFlatProbeShader. No admission
rule or config changes. This runs once per manual arm, after the audit
ends stand-down and initializes the projection runtime. The helper looks
up exact creation bytes by stage/hash, writes the named DXBC through
dumpShaderBlob and logs succeeded/failed/missing with the requested hash.
Thus a missing cache entry or a failed write is distinguishable from the
request never executing; generic-inert classification in the prior log
already establishes that this pair's bytes were retained then.

Validation: full absolute-path `build.bat --jobs 2` passed every gate,
including production FSR/DLSS DLLs, rigs and installer resource checks.
Receipt status full-pass, stamp v0.18.0-26-gb5df4ff4-dirty. Re-read the
F10 block and capture helper: one request for this PS, no new shader entry
point or duplicated census. No rendering change or flight qualification.

Two default-parallelism builds failed the heartbeat rig's combined H5
assertions. An isolated run passed. A temporary diagnostic copy under
32-way load reproduced H5.whole failures with seen=1, bad=0 and torn=0;
reader counts remained millions. This establishes sample-count starvation
for those reproductions. Lower parallelism ran all gates successfully;
no heartbeat source/test change or gate bypass was made.

Next flight uses the diagnostic build: cockpit, F10 once, then obtain
edvr_logs/shaders/ps_07B3F82100F29401.dxbc. Check the version and
`flat producer shader:` line for stage=ps hash=07B3F82100F29401. A succeeded
line confirms a saved or already-existing file; missing/failed names the
failure, and no line means the diagnostic arm did not run. Do not extend
camera-independent admission before inspecting the actual pixel shader.

### Additional flat AA report: output chain undiscovered (2026-10-02)

Evidence: `edvr-logs-20261002-230831.zip`, graphics log
`edvr_gfx_20261002_220635.log` (18234 lines). Literal v0.18.0, build
6ABED11D, linked 2026-10-01 21:31:09 UTC: the same release as the first
supporter, before the targeted capture. HEAD's release-prefix acceptance
again does not prove exact HEAD. Relevant flat model/camera/HDR route code
is unchanged from the tag. Flat profile, Odyssey-64, output 1440x900 fmt28,
captured game AA off and SS1.0. No measured scene/render size or adapter
identity; no VR runtime log, traces or shader dumps in this bundle.

The live menu changes requests between DLSS, FSR and off. Every reported
runtime window has treated=0 and temporal calls=0; 742 copy-structure
windows have copies=admitted=0 and no source VS/PS identity. Stand-down
enters for no-known-output-copy at 22:06:44. At 23:08:10 it has persisted
3300 s, with 2171 probes and 188043 frames skipped. HDR route and image
continuation counters stay zero. This differs from the earlier recognized
HDR/source chain that fails conflicting-hdr-target-or-camera.

The passive discovery final at 22:08:36.995 (frame6661, request=off) has
1488 useful frames, 1487 depth/output frames, 967654 draws, 930519 depth
draws, 7692 copies and 23587 dispatches; unknown lists/foreign-thread calls
and frame-dropped-observation counters are zero. Its selector refuses
relevant-observations-truncated: world-retained=224/224, 13 dropped world
draw observations, handoff=12/32. Camera-buffer availability is 607/11.
This proves live rendering was observed and identifies a passive capacity
limit, but cannot establish the active runtime's exact missing-copy cause.
There are no later passive samples. Earlier samples also refuse missing
HDR writes/camera or tone; their zero identities are not a draw witness.

ruled out: the menu failing to forward the AA selection, because live
discovery requests explicitly show FSR, DLSS and off.
ruled out: no rendering observed throughout the capture, because the
passive final records substantial depth/output draw work.

Open: unfamiliar final-copy/post chain, different output/context path, or
incomplete Probe recovery. Passive capacity is a separate limitation.
No manual F10 audit occurred: all details
are not-manually-armed, audit-pairs=0 and saved bytecode=0. This bundle
does not establish the earlier PS07 image-source rejection in this user.
Next: keep settings, select the intended AA mode in the cockpit, close F8,
press F10, then again after about 2 s; wait at least 5 s and collect the
log, both trace dumps and saved shaders. Full work with a recognized chain
would implicate recovery; persistent refusal plus draw/overflow witnesses
would distinguish an unfamiliar chain from capacity. Use the diagnostic
build's literal stamp and verify successful trace writes. No rendering
code, config or installation changed for this comparison.

### Sean's 1440x1080 reproduction: render/output mismatch (2026-10-02)

Local Epic `edvr_gfx_20261002_102905.log`, literal
v0.18.0-29-g31f8aee7, linked 2026-10-02 15:04:04 UTC; the exact-ref version
check passes. Relevant runtime/selector/stand-down files match HEAD
5c70e1c8 (the intervening source changes are none; commits add docs).
At 10:31:10.537, scene=1440x1080 but output=3840x2160, last refusal
render-size-does-not-fit-output. Prior treatment total 2365 is historical;
the current window has no accepted reset/history and treated-streak=0.
At 10:31:14.693 stand-down has lasted 60s and 39 probes for that mismatch.

The mapping is X=0.375, Y=0.5: not uniform, and X is below the supported
half-output minimum. flatRenderFitsOutput requires both uniform scaling
(allowing integer rounding) and each axis within 0.5..2.0. This confirms
that reproduction's refusal; it does not establish a general 4:3 ban.
Test 4:3 with a matching 4:3 backbuffer to discriminate that claim.

ruled out: the aspect/render-size gate directly causing user2's
no-known-output-copy, because frameSeen is set only when an output-resource
draw uses exact final-copy VS20F383BBAC05C031/PSDED8796049C7BB4A; a watched
frame with no such draw gets no-known-output-copy at frame end. Aspect,
projection and camera checks run later after a recognized copy. Passive
world-record truncation does not explain that runtime verdict either.

### User1's target shader received and qualified (2026-10-02)

`edvr-logs-20261002-162950.zip` repackages the previous second flight:
graphics log SHA256 22690CA230F5F9C2E91287E1FB9990AAE74D6A97C50A1EA6CDC7FBEC06FE9071,
1098516 bytes, byte-identical to edvr_gfx_20261002_202027.log. No traces or
shaders are bundled. It adds no new flight evidence.

The separate `ps_07B3F82100F29401.zip` supplies new exact bytes: 3668B,
EDVR FNV hash 07B3F82100F29401, SHA256
6c283938fea3ffa9118023c84e387850395d5b61f19e4b0b66d3e7e87e15b7de.
DXBC structure and tools/dxbc_disasm.py succeed. Its PS5.0 takes
TEXCOORD1.xy, emits SV_TARGET0, and reads only CB2[3].x, [4].zw, [5].xy,
[6].x; t0/t2 depths and t1 colour through s0/s1. It caps/compares depth
differences, chooses/interpolates colour samples, remaps alpha and may
discard. No b1, projection matrix, SV_Position input, depth output or UAV
write. Paired VS CFA91824129ECBBC independently hashes correctly (332B),
only forwarding input position to SV_POSITION and UV to TEXCOORD1.xy.
This establishes camera-constant independence, not sampled-texture
freshness or complete pixel coverage; those are not inferred from bytes.

Narrow fix: one shared exact-pair predicate admits this PS and the existing
FCFAD73924BF45B9 with the same VS. Runtime live verification must match
actual bound shader hashes to the observed pair, and all existing format9,
RT/depth/DSV/viewport/dimension/frame and HDR copy guards remain. No broad
generic-inert exemption, aspect relaxation, config key or new SRV freshness
claim. Both pairs now run existing positive and rejecting model scenarios;
the focused rig compiles/passes. All 15 recorded trace frames replay
byte-identically: their saved verified flags remain false, so those stored
recordings still reject, as they should. Full production build passed;
live verification/admission and FSR dispatch still require a new flight.

With the fixed predicate, a verifier-bit-only replay (no PS remap) changes
exactly one format9 CFA/07 draw in each of the 15 frames. All accept one
HDR copy, refuse zero copies, and select the 3440x1440 mono candidate at
the final-output copy. Five contract records include one aggregated
motion-source record; supported draw counts vary by trace. Original flags
still replay the recorded refusal with identical contract hashes.

Replay correction: EDVRFTR4 resolve markers are passive data, not draws.
The old --trace-check omitted their early return and could fabricate a
late HDR write after tone when the source became valid, falsely reporting
inconsistent-draw-order. Correcting only the ignored replay harness made
the verifier-bit-only and old PS-remap results agree. Official replay now
skips/counts resolve markers; a round-trip regression places a marker
between tone and final copy and requires the same Selected contract/hash.
The focused self-test and all 15 baseline frame hashes pass. This changes
diagnostic replay, not runtime rendering.

### User2's F10 capture: passive/runtime divergence (2026-10-02)

`edvr-logs-20261002-233240.zip`, edvr_gfx_20261002_232737.log, is new:
diagnostic v0.18.0-26-gb5df4ff4-dirty, linked 2026-10-02 14:41:48 UTC,
stamp 6ABFC2AC. Relevant observer/runtime logic matches its source base;
the later named capture does not change admission. Flat EDHM chain,
1440x900 observed output; GPU identity still absent. FSR changes to DLSS
at 23:29:38 and remains requested. At 23:32:17.547 and 23:32:23.448 F10
ends stand-down and restores Full work. All 60 runtime windows nevertheless
report treated=refused=0, last=warming-current-frame. Renderer calls/init
and jitter frames/draws/dispatches remain zero. Camera injection reports
26748 refresh calls in one Full window; observation hooks are partly live.

Passive contracts at 23:32:34.839 frame 16560 and 23:32:36.362 frame 16643
contain exact final-copy VS 20F383BBAC05C031/PS DED8796049C7BB4A on output
000001A27A2B21E0, fmt28, 1440x900, one full-viewport draw, no depth/camera.
The later sample has no observation drops and refuses no-known-tone-pass.
Earlier passive samples truncate records/edges. A passive output-class
draw is now proven; runtime copy recognition is not. No trace files or
shaders are bundled, and no flat trace dump line occurs in this log.

ruled out: no final-copy pair exists anywhere in user2's captured scene,
because the passive output contracts name that exact pair and output.
ruled out: AA selection is off at the F10 arms, because live requests are
DLSS and enabled/wanted counters are 1. This is not Sean's render/output
aspect mismatch, and does not establish user1's PS07 image-source failure.

The Full CPU windows at 23:32:20 and :25 have zero calls for other,
contract reduction, copy checks and coverage; the GPU frame scope counts
neither timed nor skipped frames. Every accepted runtime draw enters the
other scope before reducing cached state. Thus no draw passed the early
active/thread/context guards during those windows; missing downstream
shader/view metadata cannot by itself explain these zero entry counters.
Camera Map observation checks thread ownership but not context equality,
so its positive refresh count does not clear the draw's context guard.
GPU timing names context 000001A279D931C8, camera injection names
000001A2085A6B40; draw self and runtime's expected context are not logged.

Open: draw-time liveness/internal scope, thread ownership or context
identity mismatch. A bounded F10 audit will count each early gate and
sample self/expected contexts with canonical COM identities; only an
accepted scope reaches a live-versus-cached output witness. Report zero
entries distinctly. No repair before this discrimination. Passive tone
refusal and capacity limits are separate witnesses.

Implemented instrument: an F10-only 900-Present ingress audit counts scope
entries, inactive live-off/internal scopes, wrong thread, wrong context,
Paused and accepted scopes. Independent bounded witnesses sample inactive
state, wrong thread/context and one first accepted draw; at most three
copy-pair/non-null-output witnesses compare cached and actual shader/RT
state, viewport and canonical resource identities. Context witnesses use
canonical IUnknown/device identities to distinguish pointer aliases from
different objects; foreign-thread samples query self only, never runtime's
raw context. The off path adds one atomic flag load, with no queries/logs.
Armed, 5s, rearmed, complete and resize/stop summaries include zero entries,
so the guard audit cannot be hidden behind the guard it diagnoses. No
runtime guard is bypassed. No new config switch. Full absolute-path
build.bat --jobs 2 passed all gates, including production FSR/DLSS DLLs,
test rigs, installer/export/resource checks and the full-pass receipt.
Stamp v0.18.0-31-g5c70e1c8-dirty; receipt inputs SHA256
ce5929a93d9678818ec2793efac7b075ad7d82a83c4116bbedddd554b01b2d9a.
Next user2 flight: select AA in the cockpit, close F8, F10 once, wait at
least 5s and capture the new ingress summary/witness lines; a second F10
reports the prior arm before rearming. No need to infer a repair from the
passive source/tone or compute probes while runtime scopes are excluded.

### Both supporters report working; user2 DLSS is log-confirmed (2026-10-02)

Sean reports user1's FSR issue fixed, then user2 confirms working too.
User1's new-build success is human confirmation; no additional successful
flight log was supplied for that user. The exact shader, model replay and
validated fix remain the qualification evidence above.

User2 `edvr-logs-20261003-001741.zip` contains two distinct sessions:
000825 is the old -26-gb5df4ff4-dirty build and remains idle through its
startup/menu capture; 001112 is the relevant -31-g5c70e1c8-dirty build,
stamp 6ABFE356, linked 2026-10-02 17:01:10 UTC. This is the validated
pre-commit artifact whose source was committed as 0b59f0d4; HEAD's literal
version mismatch is expected, not evidence of a stale fix. The successful
log is edvr_gfx_20261003_001112.log (6508 lines).

Environment: flat EDHM chain, NVIDIA GeForce RTX 3060 (12,113 MiB), output
1440x900. Live FSR selection briefly returns to DLSS; NGX initializes at
00:11:53.944 and creates the flat HDR DLSS feature at 00:11:54.258. A
separate DLAA feature is created at output size. Driver and DLSS runtime
version are not supplied. Conflicting game AA snapshots (AAMode=4 and =0)
do not identify the final live game AA mode. No VR runtime is in this log.

At 00:17:39.686 cumulative runtime treatment is 18398, refused 2526, renderer
calls 18398 and backend-failure 0. Longest treated streak 16115. In the last
window, 147 scene HDR frames complete every route stage with zero declines;
the other 92 frames have no 3D scene. HDR is 1440x900, consumer target 720x450,
trigger VS DFED8E1C9E191BEC/PS 143AAE0597E2F7BF. Earlier full windows treat
every HDR frame (e.g. 267/267 at 00:17:34). Source image continuation counters
remain accepted 0/refused 0, so PS07's special path is not demonstrated here.

At 00:17:39.685 the active F10 arm has 3527079 entries, 3526426 accepted,
653 inactive-internal; live-off, wrong-thread, wrong-context and Paused all 0.
The output witness at 00:17:16.687 matches cached and actual VS/PS/RTV for
20F383BBAC05C031/DED8796049C7BB4A; actual colour and output canonical
IUnknown both 000002879C5C4780, viewport 1440x900. Thus the current draw gate
and output identity work. The internal exclusions are EDVR's own scope,
not an observed current game-context failure. Trace dumps are empty
(16B, frames 0/events 0); no trace or shader binaries are bundled.

ruled out: an ongoing thread/context gate or stale output/shader identity
failure in this successful session, because the new audit admits millions
of entries and the live output witness agrees with the cached state.
Outcome CLOSED for both reported AA failures on the tested build. User2's
earlier entry-path failure is not causally isolated: the new build/launch
works, no runtime guard was relaxed, and the source-exemption subroute is
unused. Do not attribute that older failure to PS07 from this confirmation.
No further rendering change or test flight requested. F10 ingress remains
a bounded manual diagnostic, with no temporary config key to retire.

### Third supporter: rc.4 no-tone-pass refusal (2026-10-02)

`edvr-logs-20261002-213417.zip`, edvr_gfx_20261002_212356.log (4790 lines):
literal v0.18.0-rc.4, stamp 6ABC3FAC, linked 2026-09-29 22:46:04 UTC.
Installation record also says rc.4. Exact HEAD version check mismatches;
this predates section 83 structural-copy admission and section 85's exact
image-source fix and ingress audit. It is not evidence of those failing.

Flat EDHM chain, output 2560x1440, GPU not identified. The flat INI sets
temporal_aa=on (TAA), model k, real_dll=d3d11_edhm.dll. No explicit
experimental.temporal_aa_before_post key, so no preserved off value blocks
the newer build's default auto route. Live F8 briefly selects DLSS at
21:27:59.511, returns to on at 21:28:00.630, and finishes in TAA.
Captured game AA snapshots disagree (AAMode=4 versus =0); final active game
AA and render/scene extent are not established. Output size is measured.

At 21:34:17.270, treated=0, refused=30046, last=no-known-tone-pass. The final
5s refusal count is 271. Renderer calls/init/backend-failures all 0: the AA
request is enabled, and the runtime attempts frame selection, but the old
selector never finds the required tone pass, so the backend is not called.
This differs from user2's idle draw-entry failure and does not identify
user1's exact PS07 image-source conflict. Automatic unknown projection
capture sees 11 pairs; manual audit pairs 0. No traces or shader binaries
are bundled.

ruled out: AA left off as the final request, because the live mode is on
and frame selection attempts/refusals accumulate.
ruled out: a demonstrated backend/driver failure in this capture, because
the backend has zero calls and fails before dispatch at no-known-tone-pass.

Recommendation: update to the fully validated flat build v0.18.0-31-g5c70e1c8-dirty
(source 0b59f0d4), retaining the user's settings and EDHM chain. Its auto HDR
and structural-copy routing supersedes this old tone-only prerequisite;
the exact filter fix is included. Do not promise a complete cure from an
obsolete capture. If still inactive on that literal build, arm F10 in the
cockpit, wait at least 5s and return a current log with the new route/ingress
witnesses. No rendering code, config or installation changed for review.

Outcome (2026-10-02): Sean reports that updating fixed it for this user as
well. This closes the third support case by user confirmation; no updated
flight log was supplied. The old capture establishes the rc.4 selector
refusal, but does not identify which newer routing change resolved it.
All three supporters now report working. No further flight requested.

## 86. Retire the flat per-draw and engine-motion A/B switches (2026-10-02)

Decision: remove `experimental.flat_per_draw_lean` and
`experimental.temporal_aa_engine_motion` from the config template, flat
profile allowlist and runtime reads. Keep both former `on` paths: make a
contract record only when the reducer needs it, and continue substituting
eligible pool-family producer draws for exact engine-record motion. Existing
entries in a player's INI become inert; no live INI is changed by this edit.

Arturbac's [discussion #71](https://github.com/characterecho-sean/edvr-unofficial-patch/discussions/71)
measured a post-discovery lean on/off/on saving of about 3.3 percentage
points of the Render thread, or about 0.9 ms per frame, with no visible FPS
gain (34-37 fps either way). On the same GE-Proton11-6/DXVK, RX 7900 XTX,
Zen 4 on-foot settlement, disabling engine-motion substitution saved about
2-3 ms GPU time, but gives moving objects depth-and-camera motion instead
of their exact engine-record motion. The latter is a quality tradeoff, not
an equivalent rendering path. These numbers do not qualify VR or Windows.

The later Epic flight log `edvr_gfx_20261002_162645.log` (build
`v0.18.1-5-g743c5dc0`) completed discovery at 16:28:46.579. In its
16:28:51-16:29:36 window, weighted contract reduction was 0.1220 ms for
3,881 calls, or 31.44 ns per call. The earlier Windows baseline build
`v0.18.0-29-g31f8aee7` completed discovery at 10:04:04.322; its
10:04:09-10:05:29 window was 0.1400 ms for 2,373 calls, or 59.01 ns
per call. That is about 47% less time per reducer call in these sampled
windows. The scenes and AA modes differ (DLAA versus FSR), and the later
run had a 90 fps cap; this comparison does not prove an FPS gain or isolate
the lean change as its sole cause.

Sean's native test found the engine-motion `off` path slightly faster but
lost accurate motion vectors. He chose exact object motion and authorized
retiring both temporary switches with their `on` behavior fixed. No new
test flight is claimed for this key removal.

Validation: the absolute `build.bat --jobs 4` passed all 122 test jobs,
the 234-key config contract, production DLLs and installer resource checks.
The first eight-job run timed out in `flat_mono_resolve_test` at 180 s;
the unchanged-source retry passed that rig in 28.6 s. Both build logs are
retained under `build/retire-flat-ab-switches-full*.log`.

## 87. Supporter Coriolis blur with flat FSR at half scale (2026-10-03)

State: investigating visual quality, not a repeat of section 85's refusal.
Bundle `edvr-logs-20261003-142110.zip` contains two graphics logs, settings
and breadcrumbs; no images, flat pixel captures, eye runs or trace blobs.
Both logs report v0.18.1, stamp 6AC026D1, linked 2026-10-02 21:49:05 UTC.
Exact HEAD check mismatches this release against our later branch. The
v0.18.1 tag is d4c6da4f; this is not the old rc.4 refusal build. Later flat
changes include reducer measurement/lean work; mismatch alone does not
establish that updating fixes the blur.

Environment: flat, NVIDIA GeForce RTX 4090, FSR 3.1.2, no real_dll chain.
No headset/runtime dependency in this flat report. EDVR requests FSR,
render_sharpness=0.0; model k is saved but is not an FSR model selection.
DisplaySettings says 1920x1080 at 144 Hz. Custom.4.4 has AAMode=0,
SSAAMultiplier=0.500000, BlurEnabled=false, DOFEnabled=0 and BloomQuality=0.
Earlier saved presets differ; backend dimensions independently confirm the
scale: at 13:52:57.781, FSR context 960x540 -> 1920x1080. Thus the game
supplies one quarter of the output pixel count. Low input detail is a
specific softness hypothesis, not proof of a station motion defect.

At 14:21:10, treated=132561, refused=1118, last=treated-jittered, backend
failures=0. Jitter/history are live; the nearby five-second window accepts
history on 323 frames with no reset. Cumulative history losses earlier do
not establish persistent resets during the reported blur. The separate HDR
route declines at hdr-route-needs-render-at-least-output (960x540 target/HDR
versus 1920x1080 output), while the other route treats with FSR normally.
ruled out: sustained AA stand-down like section 85, because this capture
has continuous treated frames and successful backend initialization.

F10 completed around 14:21:00: three projection pairs/six saved shader
stages, but no completed exact camera/copy capture. Engine unkeyed sample
binds/distinct are zero; this does not measure per-pixel station ownership.
Existing flat matched-pixel capture is gated to non-HDR DLSS/DLAA in
flat_mono_resolve.cpp, so asking for F10 alone cannot produce FSR's matched
raw/output/motion evidence. The prior Coriolis brace ownership finding came
from VR eye captures; its private-pool repair still awaits visual validation
in the kinematic arc. Neither establishes this flat report's cause.

Next comparison: same station with FSR retained, Supersampling 1.0, ship
and view still; distinguish whole-image softness from trails on rotating
station surfaces. If motion-only smearing persists at native scale, obtain
a short visual comparison before choosing motion instrumentation. No source,
live settings, build or installation changed for this investigation.

### Supplied station screenshot (2026-10-03)

Sean supplies codex-clipboard-b3869ad7-77a5-4aff-ac39-e2511798f725.png,
a 1920x1080 cockpit view of Ray Gateway. Station face/panel detail appears
smeared, with comparatively crisp cockpit edges and HUD text. This raises
the priority of station-specific temporal/motion behavior, but does not
prove bad vectors: HUD and scene can use different rendering paths, and
the screenshot's active scale/mode/time are not independently captured.
There is no temporal sequence showing whether marks trail the rotation.
The earlier log's half-scale input remains a confounder, not a sufficient
diagnosis of this localized appearance. No hypothesis is ruled out by this
single screenshot.

Refined comparison: set game Supersampling 1.0, keep ship/view still at
this station, compare FSR with F8 Off at the same scale. If native-scale
station detail clears with temporal AA off, that isolates temporal
processing as a contributor; it does not yet identify motion vectors,
history rejection or reconstruction as the faulty stage. A short clip of
both modes while the station rotates is more useful than another log alone.

## 88. Epic building shimmer: F10 remains HDR-blind (2026-10-03)

Sean reports shimmer on settlement buildings while landing. The verified Epic
log, `edvr_gfx_20261003_062849.log`, is `v0.18.1-5-g743c5dc0`, installation
profile flat. At F10 the flat runtime was DLSS mode with NGX DLAA active at
3840x2160. This is not VR evidence.

F10 at 06:35:07.463 opened pixel-capture session
`20261003_123507_462_60972_1`; it expired at 06:35:16.458 with copied=0,
completed=0, failed=0, bytes=0; it saved no pixel frames. The separate
draw-capture session `20261003_123507_463_60972_1` saved manifests for frames
206197/206198. Both have `qualified=true`, `identity_match=true`, status
`partial`, reason `copy-or-cap-refused`: 512/15337 and 512/15334 draws were
recorded, with overflow 14825/14822. Within those bounded draws,
`motion_draws=0` and `motion_complete=false`. The only identified changed
16x16 sample was point `(3456,1598)` at q172/173, VS `BFE51414CC3024B4` /
PS `DB79AE788E049DFD`. Section 49 identifies that pair as the non-pool
cockpit shell; these samples do not identify a building draw.

In installed source snapshot `build/pr68-container/pr68-merge-validation`,
`flat_mono_resolve.cpp:950-954` gates pixel capture on `!hdr && (DLSS || DLAA)`;
the capture header also excludes `R11G11B10_FLOAT`. The active HDR route was
native at 3840x2160, so removing only the gate would still not capture these
frames. Ruled out: GPU readback failure as the reason no pixel frames were
queued; HDR bypassed the copy queue. This does not explain the shimmer.

In the two pre-F10 five-second windows, the HDR route treated 301 and 295
frames. Local projection refused 0 with row/pair mismatch=0; these frame-level
counts do not establish correct building motion.
The generic draw audit is not geometry evidence. No rendering cause or fix is
qualified. Next discriminant: HDR-compatible color, depth, motion-vector,
rejection and final-image samples at a matched building ROI across consecutive
frames, before another flight; do not repeat F10 on this blind path.

## 89. Weapon-visible DLAA refusal: two alternate camera pairs (2026-10-03)

State: confirmed frame-selection refusal; shader roles need qualification.
Sean reports a different supporter loses AA on unholstering a laser rifle
and regains it on holstering. Bundle edvr-logs-20261003-154551.zip contains
one graphics log (8340 lines), settings and breadcrumbs, no shaders/traces.
Literal build v0.18.1, stamp 6AC026D1, linked 2026-10-02 21:49:05 UTC.
edvr_log --expect-build HEAD accepts the v0.18.1 release prefix at source
0fa51a88; this is not an exact source-commit identity proof. The exact
weapon exception is unchanged between the release and current source.

Flat mono, RTX 3050, 1920x1080, native-resolution NGX DLAA preset K; no
headset/runtime in this path. Saved INI asks dlss, but NGX initialization
and live warnings name DLAA. Selection remains requested during refusals;
no evidence of an automatic menu mode change to Off. Earlier loading and
post-chain intervals have separate no-3d-scene/no-known-tone-pass causes.

At 15:43:24.111/35.156, firstBad hdr-camera-changed is exact pair
025B4B9FF54622ED/46F92DC71BF8DFA5. At 15:43:45.156/55.156 it is
9AEC596A2B036EA6/3789CA2062E196FB. Both compare against scene pair
68DDDEF04D9894AF/06332CA168B6DA63. Same HDR RTV, DSV/depth identities,
1920x1080 dimensions and current-frame b1 epochs; changed camera words
are XY scale and near 0.025 -> 0.0675, with rows 274/275 unchanged.
At 15:44:08.543 the window has 272 camera conflicts, streak=0, last=
conflicting-hdr-target-or-camera. By 15:44:18.553 it accepts 300 history
frames with no new refusals; final treated=40281/refused=3500, streak=4505.
The log has no weapon-event timestamps, so onset/holster correlation comes
from the supporter, not a recorded input event.

Section 57 admits only 88DCF1164C640EC3/494506A63091DF8C as an alternate
weapon camera. New 025B VS is listed as weapon/tool material, but this PS
companion has no exact recipe (the known companion is C5A5C7E8216CB9AF).
9AEC/3789 has an exact b1 ForwardColumns recipe, but its VS is a generic
billboard/flare source also used by witchspace. A matching narrow camera
alone does not prove that effect's ownership. Do not admit either from
the reported weapon name or globally ignore changed cameras.

Evidence gap: original F10 camera probe only observes the old rifle pair.
F10 at 15:44:12/46/50 wrote three trace files on the user's machine; none
is in the archive. Producer shader capture saved VS 025B, but the new PS
and effect bytecode are not demonstrated. Global glare_shader_dump is
refused by the flat profile allowlist; it is not a working capture path.

Diagnostic implemented: extend bounded flat stage/hash capture and F10 cached
shader requests to both new pairs; observe two distinct conflict frames
per exact pair, keeping actual shader identity and CPU shadow checks.
Per-pair budgets prevent the mesh from starving the effect capture; report
zero-observation and partial/mismatched results as well as complete ones.
No camera/renderer admission, config key or live setting changes. Next
flight: restart on the diagnostic build, draw
the rifle, F10 while the AA refusal is present, wait at least five seconds,
then send the shader/trace folders together with the fresh flight log.

Validation: absolute build.bat --jobs 2 passed all gates, including 117
pooled and 5 quiet jobs, the new per-pair camera probe tests, 46 existing
trace frame replays, the 234-key contract and installer resource checks.
The sandboxed run stopped at run_jobs process-tree validation; the
unchanged-source escalated run passed. Log:
build/weapon_camera_diagnostic_full_build_escalated.log. Full-pass receipt
inputs SHA256 20b1d95485cf1cfc7199de8ba96c84a3b5a7070df96c6fdbddaf35a5bc172783.
Validated flat installer: v0.18.1-10-g00d4f738-dirty, 79570944 bytes,
SHA256 43ACE66D4995B49DBFE4DF79E091441D6E5D3D297BFB733DFAB66F3825ECAA18.
This is a diagnostic artifact, not a claimed rendering fix or flown change.

## 90. HDR building capture preparation (2026-10-03)

Sean authorized the capture work after section 88. This changes diagnostics;
the rendering cause remains unqualified. Target: Epic's Windows flat profile,
native 3840x2160 NGX DLAA on the HDR route. No VR runtime or headset is involved
in this evidence; this does not qualify the VR route or other DLSS versions.

Discriminants for the next flight:

- Missing or wrong object motion: a building window has non-sky depth, but its
  engine slot/freshness, reconstructed motion or rejection disagrees with the
  frame camera and the building's displacement. A sampled draw that changes
  that window can identify its shader family; an omitted draw cannot.
- An integration/history problem: building raw input remains steady while the
  backend output changes excessively, with matching source identity, adjacent
  live frames and valid motion. The finish result establishes whether the
  backend fluctuation reaches the HDR image.
- Aliasing already in the scene input: the same building detail fluctuates in
  the pre-resolve color. Depth, motion and rejection distinguish missing
  coverage from fine geometry or material detail; color alone cannot.
- A later post-processing problem: the captured HDR finish is stable while the
  visible final image shimmers. HDR capture is before tone mapping and cannot
  prove the state of every later effect.

Implemented; combined full validation passed. Ready for the Epic test:

- HDR DLSS/DLAA pixel capture uses a centered region, at most 2048x1152. At
  3840x2160 its origin is (896,504). Save color, prepared depth, emitted
  motion, rejection, raw backend output, finished game HDR target, engine
  slots, pool and scene constants. Preserve source coordinates and linear
  radiance; the finished target is before tone mapping. Record the actual
  refusal overlay and distinguish expected output-format quantization.
- Queue a pair of live frames before readback; allow a second pair after the
  spacing interval. Report frame adjacency rather than assuming it when a
  resolve is refused. Keep the cumulative 384 MiB limit. Cropped engine replay
  retains full render coordinates and is tested against full-frame replay.
- Draw capture samples the selected depth's scene and motion candidates over
  the frame, with at most 256 records per class and a 256 MiB limit. Eight
  windows lie inside the central HDR region. First-eight and every-64th
  admission uses complementary frame phases; pool snapshots are also spaced.
  Explicit skip, quota and refusal counts make this partial coverage. An
  omitted draw is not evidence of unchanged pixels or missing building motion.
- WARP fixtures check two simultaneously pending HDR frames, source bytes,
  nonzero region origins and retained engine inputs. The draw policy test
  includes more than 15000 draws, late motion producers and saturated quotas.

Next flight: keep the shimmering buildings in the center of the screen,
press F10 while the effect is visible and keep flying for several seconds.
Verify the new build first, then use the capture's exact region coordinates
and frame identities. No new setting or rendering compensation is warranted
by the previous log. FSR/TAA matched-pixel capture remains unsupported and now
reports that explicitly instead of silently waiting for expiry.

Validation on the pre-merge HDR capture tree: all 122 gates passed (`build/hdr-building-full-retry.log`),
including the WARP capture fixtures, offline tools, config contract and actual
installer-resource checks. DLSS SDK 310.9.1 verified against its pinned hash.
The first sandboxed build stopped at the process-cleanup self-test and missed
the cached SDK; an unsandboxed retry with the explicit SDK path passed.
Combined camera-probe/HDR capture tree: all 122 gates passed again in
`build/hdr-building-combined-full.log`, with full-build receipt digest
`e0f45f9ec70a3a7e4c7e113ab5591e08d9f08b0652b40664e84a37338e9d9bfc`.
Epic's installed DLSS 310.9.1 runtime matches the SDK's pinned SHA-256;
preserve it and `edvr-flat.ini` for this test. The install-only clean-version
promotion uses the matching receipt. No flight has qualified the new capture
or the building-shimmer cause yet. Section 91 records the subsequent flight.

## 91. Epic settlement flight: F10 discovery cost and HDR evidence (2026-10-03)

Verified `edvr_gfx_20261003_075004.log` against installed
`v0.18.1-12-g41b9838c` (6AC106D5), flat, native 3840x2160 DLAA. Sean reports
poor settlement performance and took F10 in the cockpit and on foot. The
existing motion switch had been on; previous verified 743c5dc0 windows also
substituted about 12000 motion draws per frame. Its retirement does not explain
a newly enabled motion path in these windows.

F10 was at 07:53:28.160 and 07:54:01.736. Both pixel bursts completed 4/4,
engine-complete, zero failures, 333490176 bytes each. Pixel work finished at
07:53:29.603 and 07:54:02.788. Source regions visibly contain buildings;
cockpit frame pairs are 185439/440 and 185455/456, on-foot pairs 186642/643
and 186658/659. No additional flight is needed merely to repeat this capture.

| Window | Present p50 | GPU frame p50 | Motion-wrapper D3D calls/frame |
|---|---|---|---|
| 06:35:05, prior 743c5dc0 flight, before F10 | 16.48 ms | 10.63 ms | 19185 |
| 07:53:25, new flight, before F10 | 18.76 ms | 10.60 ms | 19856 |
| 07:53:45, new flight, after F10 | 40.75 ms | 31.77 ms | 100505 |

The new pre-F10 reducer is 0.511 ms / 16711 calls, about 31 ns/call; the
prior reducer is 0.500 ms / 16341 calls, also about 31 ns/call. The views are
not a controlled A/B, so the 16.48 versus 18.76 ms baseline difference does not
prove a regression. There is no measured reducer regression here.

Initial passive discovery completed at 07:50:47.932. Each F10 re-arms it for
up to 120 s / 12000 useful frames. While it is active, source explicitly
disables lazy motion batching and flushes substitution; the per-draw observer
is also active. Discovery adds 3.357-6.784 ms per clocked frame in the sampled
post-F10 windows; wrapper D3D calls rise fivefold for similar substituted draw
counts (12445 before, 12315 after). GPU resolve remains about 0.99-1.01 ms.
These are substantial diagnostic costs; the whole frame-time increase is not
isolated causally. This behavior is unchanged between 743c5dc0 and 41b9838c.
The log ends before discovery completes again, so no quiet post-F10 settlement
window is available. Clocked CPU totals are sampled one frame in 16 and include
an estimated profiling floor; they are not every frame's net EDVR cost.

Ruled out: continuing HDR pixel copies as the cause of the 30-second slowdown,
because both four-frame bursts finish within 1.5 s and their active paths stop.
Performance comparison: stay in the same view, avoid another F10, and wait for
`flat temporal: passive discovery complete` (up to two minutes after the last
F10) before comparing frame times. Do not revert PR72 from these captures.

On-foot building input has phase-dependent aliasing: adjacent scene-H preview
change averages 4.68/255, whereas NGX output is 0.10/255 and finished H
0.14/255. In cockpit same-phase frames 16 apart, a wall's input is nearly
unchanged (0.005/255), but backend/final HDR changes about 2.15/255. This is
temporal-output variation, not proof of the perceived shimmer's cause: F10
re-arms state, intervening frames are unobserved, and H precedes tone mapping.
Both positions are essentially static (emitted motion below 0.0005 px), so
landing-motion correctness remains unqualified.

Draw sessions are partial; the actual t33 pool is 5505024 bytes (336-byte
stride, 16384 entries), exceeding the draw snapshot's 4194304-byte cap. The
24/23 on-foot refusals, `t33-range-or-size`, are this diagnostic limit, not an
invalid engine pool. Pixel capture retains the complete pool.

Offline engine replay predicts far more stale/depth rejection than the actual
GPU bytes because it omits the enabled steady-detail branch: the shader can
accept a slot-depth mismatch using the camera term if previous depth confirms
it. The capture omits that previous-depth texture and the actual steady flag;
its replay only knows the distinct `static_scene` policy. Logs confirm steady
depth checks ran, with zero skips. On-foot frame 186642 has 174526 valid-slot
depth mismatches, of which the GPU accepts 174411; median absolute depth delta
is 6.49e-8. Do not infer motion failure from replay's stale-branch counts.
Exact per-pixel confirmation needs previous depth and the actual prep flags
in the capture before a further diagnostic flight. Captured GPU rejection in
the on-foot building region is about 0.019%; this alone does not validate motion.

## 92. Weapon refusal reproduced; quiet performance recovers (2026-10-03)

Verified `edvr_gfx_20261003_082043.log` against installed
`v0.18.1-12-g41b9838c` (6AC106D5), Windows flat, native 3840x2160 DLAA.
Sean drew a weapon, saw world AA turn off, F10 at 08:23:54.958, holstered
during the two-minute wait, then took cockpit F10 at 08:26:07.549 and waited
again. Weapon input timestamps themselves are not logged.

Performance: final passive discovery completes at 08:28:07.568. Quiet HDR
windows at 08:28:15.299/20.302/25.707 have zero declined frames and discovery
0.000 ms. Present p50 is 15.66/15.65/15.95 ms (roughly 63-64 FPS by median
frame time); GPU frame p50 10.46/10.31/10.56 ms, resolve 0.98/0.98/0.99 ms.
Wrapper D3D calls are 19826/19820/19817 per frame, over approximately 12435
substituted draws. This recovers section 91's pre-F10 submission regime and
is comparable to the previous 743c5dc0 cockpit's 16.48 ms / 10.63 ms. The
large sustained-looking slowdown after F10 is not present once discovery
ends. These are not controlled pre/post-PR72 views; no reducer regression
is established. CPU timings still use one-in-16 clocked frames and include
profiling cost; GPU spans and Present quantiles have their own sample counts.

Weapon diagnosis: exact VS `025B4B9FF54622ED` / PS `46F92DC71BF8DFA5`
caused the first HDR camera conflict in frames 171619/171620. Draw sequences
16773/16856 equal first-bad-seq, actual hashes match and HDR RTV, depth, DSV
and named b1 identities are unchanged. Camera XY coefficients scale by
1.231279 and near depth changes 0.025 -> 0.0675; pose rows 274/275 match
byte-for-byte. This is an alternate projection at the same pose. Treatment
stays at zero in the weapon interval, then resumes after the reported holster.

Ruled out: a missing projection recipe as this frame's AA blocker, because
the generic recipe is prepared on all 900 observed offending-pair draws and
the reference reports canonical=900, unmatched=0. Adding an exact recipe
alone would not remove the HDR ownership conflict.

Shader bytecode now exists: pooled packed vertices, t33 stride336/t38 stride48,
position through b1[270..273]; PS texture arrays t2/t3 and structured t1 stride96,
color output, no SV_DEPTH. VS declares no b0, so an unbound b0 is expected;
the older rifle probe's aggregate partial flag does not invalidate complete
b1 and actual-shader evidence. Captured depth state has depth writes OFF,
stencil REPLACE with ref/mask 0x04. The older qualified rifle pair writes
depth; its strict-depth fallback does not qualify this color-only pass. The
existing first-person resolver mask uses 0x10. Do not globally admit changed
cameras or assume the 0x04 mark is exclusive and survives until resolve.

The old 88DCF/494506 rifle pair is observed in the first audit but is not its
first camera conflict; new 9AEC/3789 effect is never observed during this F10
and remains unqualified. The cockpit has no offending 025B/46F pair.

First F10 pixel arm expired with zero copies; draw arm expired after 900
unqualified frames while selection stayed conflicting-hdr-target-or-camera.
The camera/shader probe works during refusal, but matched HDR pixels do not.
Next discriminant is passive before/after color and stencil footprint for this
exact draw and stencil survival to resolve, captured even on a refused frame.
If an exact-pair/state exclusion is supported, GPU tests must prove it prevents
borrowing world-depth history over weapon color and retains refusals for
mismatched hash/state. No rendering admission change is qualified yet.

Cockpit pixel capture completes four frames 179362/363 and 179378/379, engine
inputs intact. Its building region's same-phase final-H preview drift is
0.179/255 (0.11% of pixels change at least 4/255), versus 2.285/255 (15.60%)
in section 91's prior cockpit. Source-H drift is 0.009/255. This short,
essentially static sample shows less backend variation; it does not prove
shimmer gone during landing or after tone mapping. Known draw-pool and
steady-depth replay limits in section 91 still apply. No build or live setting
was changed for this analysis.

## 93. Refused-frame weapon footprint capture (2026-10-03)

Sean authorized the next instrument after section 92. Hypotheses to separate:
the exact weapon draw marks newly changed color with stencil 0x04; the mark
already belongs to other surfaces; a later clear or draw removes or expands
it before the HDR consumer. Before/after color, depth and stencil, followed
by pre-consumer and frame-end observations of the same resources, distinguish
these cases. `night_vision.cpp` also recognizes stencil write mask 0x04 with
REPLACE, so the bit alone is not evidence of exclusive weapon ownership.

Implemented and validated: an independent F10 arm for VS
`025B4B9FF54622ED` / PS `46F92DC71BF8DFA5`, including frames whose camera
ownership refuses AA. Two bounded draw samples retain resource/state
provenance. A lower-center ROI includes the held-weapon region; omissions
outside the ROI remain explicit, and no observed change cannot exclude a
footprint outside it. Depth-stencil uses a whole-resource typeless GPU mirror
and shader extraction into compact depth/stencil planes; boxed partial
depth-stencil copies are invalid. All pending allocations are budgeted.
Offline analysis measures preexisting marks, changed color/depth and mark
survival, without equating unchanged color with absence of raster coverage.

No AA admission, masking, motion behavior or live setting changes are
authorized by this evidence. Target is Windows flat Epic at native 3840x2160,
DLAA with the installed DLSS runtime preserved; this instrument does not
qualify VR. Next test after build/install: draw the weapon until world AA
turns off, press F10 and keep it drawn through capture completion, then
holster. Performance comparison still requires waiting for F10's existing
two-minute passive discovery window to end. Full build and all gates passed;
clean-commit DLL promotion passed. Epic flat installed and verified as
`v0.18.1-15-g06e9225c` (code commit `06e9225c`). Live `edvr-flat.ini` and
`nvngx_dlss.dll` SHA-256 values are unchanged. This build is NOT FLOWN.

The WARP fixture uses the production snapshot/extraction path: 16 changed
color pixels, no selected-draw depth change, 15 new stencil-0x04 marks,
12 surviving to the consumer and none after the final stencil clear. Four
unrelated depth pixels change later. Tests also verify game bindings survive
capture, alias DSV clears are recorded, later-frame clears are excluded,
wrong-pair/off-arm rejection, no-match expiry and a producer-emitted missing-
consumer partial manifest. Both offline fixture gates and parser self-tests
pass. These fixture results validate the instrument, not live AA admission.

## 94. Live weapon capture: stencil 0x04 is already global (2026-10-03)

Verified `edvr_gfx_20261003_092909.log` against installed
`v0.18.1-15-g06e9225c` (6AC11E84), Windows flat Epic, 3840x2160, preserved
DLSS 310.9.1. Sean completed the draw-weapon/F10/hold/holster test. F10 arms
at 09:33:16.385; both four-stage captures complete by 09:33:16.830 with
unsupported=0. Session is `flat_weapon_footprint/20261003_153316_385_54728_1`.
Frames 105372/105374 each observe the exact pair once; their selected draw
sequences 24219/23400 equal first-bad-seq. The HDR consumer four draws later
reports `conflicting-hdr-target-or-camera` for both samples.

Runtime treatment stays at 7168 through 09:33:20/25 while refused frames rise
1859 -> 1990 and accepted history remains zero. By 09:33:30 treatment resumes
(7229 treated, accepted-reset=1/history=60), then 09:33:35 adds 119 history
frames without more refusal. Recovery between 09:33:25 and 30 is consistent
with Sean's holster after the ten-second hold; the input itself is not logged.

The lower-center ROI is x896..2943/y1008..2159, 2,359,296 pixels (28.44% of
the target). Before/after color, depth and stencil changes are all zero in
both samples, as are subsequent changes through the consumer and pre-Present
snapshot. There are no recorded stencil clears. Every sampled pixel already
has bit 0x04 before the exact draw and retains it; new marks=0. Raw stencil is
only 0x04 or 0x14. The 0x14 region is lower-right and clipped by the ROI edge
(x2199..2943/y1394..2159 in the first frame; x2203..2943/y1382..2159 in the
second). It suggests existing first-person marking but does not prove this
draw owns those pixels. Camera XY scale ratio remains approximately 1.231279,
near depth 0.025 -> 0.0675 and pose rows match. State is depth GEQUAL with
no depth writes, stencil ALWAYS/REPLACE, ref/write-mask=4 and read-mask=0.

Ruled out: stencil 0x04 as an exclusive local weapon mask, because every
sampled pixel carries it before the draw. No newly marked footprint exists
here to test survival. Unchanged bytes cannot exclude off-ROI coverage,
rejected fragments, disabled color writes or equal-value overwrites. No AA
admission exception is qualified. Next discriminants are wider right-edge
coverage and actual whole-draw contribution, including draw arguments,
blend write mask, scissor and passed-sample evidence. These should separate
the remaining causes in one capture rather than one flight per hypothesis.

Passive discovery ends at 09:35:16.401. Clean windows at 09:35:26.025/31.042
have discovery=0, Present p50 16.32/16.91 ms, GPU frame p50 11.15/11.28 ms
and resolve 1.00/1.01 ms. HDR windows at 09:35:25.763/30.769 treat all
295/286 frames with no local refusals (roughly 59/57 average FPS). Wrapper
D3D calls are 25674/25679 per frame over approximately 14557 substituted
draws, more geometry than section 92's cockpit. This establishes recovered
late-flight performance, not a controlled PR72 comparison. No renderer,
installed build or setting was changed during this analysis.

## 95. Wider weapon capture and actual draw visibility (2026-10-03)

Sean authorized the next instrument after section 94. Hypotheses and
discriminants: color contribution outside the old ROI (full-width lower
snapshots); a nonempty draw whose samples fail depth/stencil (draw arguments,
pipeline work and zero passed samples); disabled color writes or unchanged
overwrites (effective blend state and passed samples with no color change);
empty or suppressed work (arguments, pipeline counters and predication).
None of these alone establishes safe temporal history ownership.

Implemented and validated: extend the existing F10 capture to full source
width and the lower 1152 pixels, preserving the 384 MiB memory/disk guards
and serial two-sample readback. At native 3840x2160 with R11 HDR this fits
the budget and covers both horizontal edges. Record effective blend,
rasterizer/scissor, topology, predication and draw arguments. Auto/indirect
counts remain explicitly unknown when the original call does not provide
them. Bracket only the real game draw with occlusion and pipeline-statistics
queries; snapshot extraction runs outside that interval. Later Presents poll
without flushing or waiting, and unavailable/failed/timeout data are distinct
from a measured zero. Schema 2 retains offline support for schema 1 captures.

Microsoft defines occlusion as samples passing depth/stencil, not color
ownership; pipeline statistics separately report shader invocations:
[D3D11 query types](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query).
Tests must distinguish visible, depth-rejected and color-write-disabled draws,
include a right-edge contribution outside the old ROI, and prove extraction
compute dispatches do not enter the selected draw's statistics. No AA
admission change or new config key. Full build and all gates passed. Six
right-edge WARP cases distinguish visible, depth-rejected, color-write-disabled
and empty draws, plus unavailable/timed-out queries. They verify nested game
occlusion queries retain their result and capture CS invocations stay outside
the draw statistics. Existing complete/partial capture gates also pass.
Clean promotion passed; Epic flat installed and verified as
`v0.18.1-18-g83938927` (code commit `83938927`). Live `edvr-flat.ini` and
`nvngx_dlss.dll` hashes remain unchanged. This diagnostic build is NOT FLOWN.

Next test uses the same weapon that reproduced the conflict: draw it until
world AA turns off, F10, hold it drawn ten seconds, then holster. Wait two
minutes before comparing performance. No per-weapon live test matrix.

Sean explicitly requires a solution that does not need him to test every
weapon type. The exact shader pair is a diagnostic witness, not the final
support boundary. The intended fix follows the shared first-person camera
and render phase, with actual pixel/history ownership separated from the
world. Automated GPU tests cover pass variants; a representative live test
confirms the game route. Do not replace the current conflict with a growing
per-weapon hash whitelist or assume global stencil 0x04 identifies weapons.

## 96. Real draw confirmed; lower image stays unchanged (2026-10-03)

Verified `edvr_gfx_20261003_101515.log` against installed
`v0.18.1-18-g83938927` (6AC12887), Windows flat Epic, 3840x2160. Sean
completed the weapon diagnostic. F10 arms at 10:19:57.869; both four-stage
captures finish by 10:19:58.339, unsupported=0. Session is
`flat_weapon_footprint/20261003_161957_869_59304_1`, frames 66718/66720.
Each frame allocates 315129856 bytes. No two-minute performance wait is
needed for these captures; this analysis makes no new performance comparison.

Exact draw: DrawIndexedInstanced, six indices starting 6623712, base vertex
3096903, one instance starting 37036. Triangle-list statistics report two
IA primitives, four VS invocations and two clipper primitives. Whole-draw
occlusion and PS invocation counts are 55365/55236, with CS invocations=0.
Both queries and explicit brackets complete successfully. Cull=BACK,
scissor disabled, predication unbound, sample mask all, RTV0 write mask=15
and blending enabled. This is a real two-triangle material draw; the record
does not establish that it is the weapon mesh itself.

Ruled out: empty draw, whole-draw depth rejection and a disabled RTV0 write
mask, because nonzero primitives/samples/PS work and write mask 15 are
measured. These do not rule out a no-op blend or sampled zero alpha. The saved
PS outputs o0.xyzw; t3's scalar supplies alpha and multiplies RGB, so shader
execution is not by itself proof of visible color changes.

ROI now spans x0..3839/y1008..2159, 4,423,680 pixels (53.33% of the frame).
Color/depth/stencil changes remain zero before/after and through the consumer
and pre-Present snapshot in both frames; there are no recorded clears.
Stencil 0x04 preexists on every sampled pixel. Bit 0x10 preexists on
480726/477369 lower-right pixels, bounding boxes x2190..3397/y1392..2159
and x2188..3393/y1396..2159, and is unchanged. Thus the selected draw has no
measured effect on that known lower first-person region. The upper 1008
rows remain unsampled; passing samples cannot identify their location.

The alternate camera still has XY scale ratio approximately 1.231279,
near depth 0.0675 versus reference 0.025 and identical pose rows. It writes
no depth and replaces stencil 0x04. First-bad sequences 14797/14793 equal
the selected draws. Consumers four draws later report
`conflicting-hdr-target-or-camera`. No simple ignore/whitelist exception is
qualified, and stencil 0x04 remains ruled out as an exclusive marker.

The intended shared-path design remains actual per-draw coverage with local
history isolation, validated by automated pass-variant tests. Conservative
coverage can include equal-color/transparent fragments without depending on
weapon type, but shader substitution, blend preservation, raster phase and
backend history semantics must be proven before admission changes. A reactive
mask must not be assumed to guarantee zero history for every backend. No
renderer, installed build or live setting changed during this analysis.

## 97. Guarded shared late-overlay isolation (2026-10-03)

Section 96 supplies enough evidence to implement the shared-path mechanism
and prove it with automated GPU tests before another representative flight.
The exact shader pair remains a diagnostic witness, never an admission list.
No additional cropped capture or per-weapon live matrix is planned.

Proposed contract: before the first supported alternate-camera, color-only
HDR draw, retain clean pre-overlay HDR. Append a private coverage output to
the original pixel shader while preserving its original outputs, clipping,
depth/stencil tests and color blending. Union passing fragments across the
late overlay suffix. Every later HDR writer before the consumer must satisfy
that contract; an ordinary world write, copy, clear, unsupported shader or
binding, or unproven raster phase keeps the existing frame refusal.

The temporal backend receives clean pre-overlay color. The final HDR finish
uses the game's post-overlay color under aligned coverage and resolved world
color elsewhere. A reactive mask or a final current-color override alone is
insufficient: opaque DLSS/FSR history could otherwise ingest the overlay and
carry it into later frames. Keep raw post-overlay input and live HDR output
separate from clean backend input, avoiding SRV/RTV feedback.

Implemented and installed, not live-qualified. Targeted tests and full
validation passed. No new config key or diagnostic removal. Missing or
declined HDR consumers refuse before the final LDR copy can call a backend.
The suffix seals at the ready HDR consumer; later HDR writers retain the
existing post-consumer latch. PS UAV binds, depth/stencil or producer writes,
unknown copies/clears and scene-camera writes fail closed while it is open.

Initial environment: Windows Epic flat, upstream camera injection, centered
same-pose alternate projections, single-sample R11G11B10 HDR, and a free
private MRT slot 7. The latest capture is 3840x2160; installed DLSS is
310.9.1. Legacy injection, off-center or unproven cameras and unsupported
target layouts retain refusal. This scope does not qualify VR or Frontier.
The coverage shader cache is bounded to 2048 structurally supported shaders
and 32 MiB of patched bytecode; reaching a cap retains refusal. Overlay draws
that produce engine-motion metadata also retain refusal, so clean color is
never paired with motion written by the excluded overlay.
Private clean HDR plus coverage allocations are capped at 128 MiB and reused
across frames. The production R11 HDR route at 3840x2160 uses 41,472,000 bytes
for these textures; exceeding the budget retains refusal.
The resolver also retains a raw post-overlay R11 copy for feedback-free
composition. Total additional active-overlay scratch is 74,649,600 bytes
(about 71 MiB) at this size; ordinary frames do not create these allocations.

Automated evidence: the WARP layer test marks 32/256 passing fragments after
discard, depth and stencil rejection while original HDR and D24S8 bytes match
an uninstrumented draw exactly. A second shader unions to 96 pixels; the next
frame reuses the same private textures, refreshes clean color and clears old
coverage. An alpha-zero draw marks all 256 pixels while HDR/depth/stencil stay
byte-identical. Occupied MRT7, PS depth output, linkage, predication, OM UAV
and unsupported conditional control flow refuse.

Both SDK stubs receive clean 20/30/40 pixels where the game has 200/10/10.
The final HDR composite matches the game's bilinear color at all four
neighbors with phase (+0.25,-0.25), preserving resolved world elsewhere.
TAA has covered and zero-mask clean-input controls. The next unmarked frame
uses backend world output; an incomplete input pair preserves H and never
calls the backend. Reducer and trace replay tests cover protected suffixes,
later HDR/depth writes, retained prior conflicts, actual phase/pose mismatch,
failure/seal markers and post-seal unrelated writes. Updated source wiring
gates retain isolation and verify the distinct clean/raw copies.

The 5s `flat late overlay` line reports planned and fully marked draws,
isolated consumer frames and refusals. A route that never ran is distinct
from one that completed. Its shader/target/blend binds are verified before
the original draw; incomplete brackets retain refusal. No more diagnostic
flight is required before installing the validated fix.

Full validation passed with receipt fingerprint
`76efe4d51af3882ae8aed6212be5c1aa7cc480c3b5020ee46aa515e410d38506`.
Committed as `087501bb`, fast-forwarded main and verified the push. Clean
DLL-only promotion passed. Epic flat installed and verified as
`v0.18.1-21-g087501bb`; live `edvr-flat.ini` and `nvngx_dlss.dll` hashes are
unchanged. This fix is BUILT, NOT FLOWN.

Next representative test: reproduce with the same weapon, hold it drawn ten
seconds, then holster for ten. Check that world AA remains active and that
`flat late overlay 5s` reports fully marked draws and isolated consumers.
Take F10 if world AA still turns off or another visual defect appears. A
two-minute wait is only needed for a performance comparison, not this test.
No per-weapon test matrix and no new diagnostic flight before this install.

## 98. Live route activation failure (2026-10-03)

Verified `edvr_gfx_20261003_110718.log` against installed
`v0.18.1-21-g087501bb` (6AC13580), linked 17:04 UTC. Sean reports world AA
still turns off with the same weapon. Representative 5s window at 11:10:49:
planned-draws=417, fully-marked-draws=0, isolated-consumer-frames=0,
refused-frames=417, reason `overlay-draw-scope-incomplete`. Holstered windows
return to treated HDR/history, including a 744-frame streak at 11:11:24.

Ruled out: the camera/phase admission never runs, because the live plan
counter increments on hundreds of draws per window. The actual coverage
bracket does not run on those ordinary draws: all seven flat draw thunks
still gate begin/end solely on `weaponFootprintStarted`. That ties the real
fix to the F10 instrument and leaves `overlayPlanned` unexecuted. Correct
the call-site gate and test every original draw bracket with F10 unarmed.

A second measured refusal appears only when F10 arms the bracket:
`unsupported PS UAV/structured operation`, twice in the 11:11:09 and
11:11:39 windows. The exact diagnostic pair's captures complete at
11:11:37.783/37.955, so this is not a readback timeout. Inspect its saved
bytecode and authoritative opcodes before allowing any operation. A safe
SRV structured read and a UAV write need different treatment; retain UAV,
atomic and unknown-control-flow refusal. No additional flight is needed to
distinguish these two implementation failures.

Saved `edvr_logs/shaders/ps_46F92DC71BF8DFA5.dxbc` disassembly confirms
`dcl_resource_structured t1, 96` and `ld_structured` from t1, no UAV
declaration, store or atomic operation. Its opcodes 162/167 were rejected by
the patcher's blanket `op >= 143` guard. It also has balanced IF/ELSE/ENDIF
(31/18/21) and one terminal top-level RET, which the initial flow guard
would reject next. Microsoft DirectX-Headers confirms the opcode mapping.
The correction admits validated raw/structured SRV reads and balanced
branches with one final top-level return; UAV operands, stores, atomics,
early returns, loops, calls and malformed flow remain refused. Test the
captured PS's actual patched shader creation before another live flight.

Implemented normal-draw activation through one shared policy: footprint OR
planned overlay. Regression tests cover all four input cases and all seven
begin/original-draw/end hook branches, with removal, ordering and indirect
offset mutation controls. They fail the old diagnostic-only wiring.
Targeted temporal self-tests pass. WARP tests pass for read-only structured
and raw SRV loads with balanced branches, preserving original color/depth/
stencil and marking coverage. UAV stores/writes/atomics and unsafe return
flow remain refused, including actual SRV-versus-UAV load operand checks.
The exact captured 2008-byte PS patches to 2076 bytes and WARP creates it
successfully. Full validation passed, including the GPU rigs and installer
gates, with receipt fingerprint
`4cb7f4dc02591042e3c501aec845e7d912784ace9b592e7ad4856a2833941467`.
Committed as `90d0c3d8`, fast-forwarded main and verified the push. Clean
DLL-only promotion passed. Epic flat installed and independently verified
as `v0.18.1-23-g90d0c3d8`; live `edvr-flat.ini` and `nvngx_dlss.dll` hashes
are unchanged. These corrections are BUILT, NOT FLOWN.

Read-only suffix audit finds no additional confirmed blocker. F10 trace
dumps at frames 56778/58972 contain zero frames/events (16-byte headers,
four skipped slots). The frame-58973 manifest identifies selected seq 17735
and consumer 17739 with the same H/depth, but has no draw records for the
three intervening sequences. Their writer roles remain unknown; runtime
suffix checks must still prove them in the representative test. Do not
assume the snapshot ROI establishes their ownership or waive those checks.

Next representative test: same weapon drawn ten seconds, then holstered
ten seconds. Check world AA and the 5s fully marked/isolated counters. No
two-minute wait or weapon matrix is required; take F10 only if AA still
turns off or another visual defect appears. Verify the next log against
the literal installed `v0.18.1-23-g90d0c3d8`, not the later docs-only HEAD.

## 99. Marked weapon, later resource refusals (2026-10-03)

Verified `edvr_gfx_20261003_113932.log` against installed
`v0.18.1-23-g90d0c3d8` (6AC13A91), linked 17:25:37 UTC. Sean reports the
same weapon still disables world AA and took F10. At 11:42:07: planned=305,
fully-marked=305, isolated-consumer-frames=0, refused-frames=305, with
`overlay-scene-constants-written` 194 and
`overlay-unknown-write-or-command-list` 111. F10 completes frames 48487 and
48489, with two complete captures and zero unsupported captures.

Ruled out: the ordinary-draw coverage bracket never runs or the captured
weapon PS cannot be patched, because every planned draw is now fully marked
outside F10 as well as during it. The failure occurs later in the suffix.
Trace the specific operation and affected resource before changing either
refusal: constants can change future draw interpretation, while a generic
copy/clear hook may know its destination. Retain true HDR/depth writes and
unknown command-list refusal. No additional flight requested yet.

Source audit confirms the constant-upload refusal is `res==namedConstants`,
not a write to H or scene depth. World rows are already copied into
`namedCamera`, target/selector records and resolve `f.camera`. Engine source
rows and scene/previous GPU inputs are private snapshots. Subsequent camera
uploads cannot mutate them; later draws and engine producers retain their
own admission checks. The specific log reason therefore confirms a stale
blanket invalidation rather than loss of those saved inputs.

The second reason is emitted for every kCopy/kClear/kResolve substitution,
including known-destination UpdateSubresource before destination inspection.
It does not establish a write to protected H/depth. Correct it by checking
known destinations/views at all mutation hooks while preserving substitution
flush semantics, null lookup refusal, actual H/depth writes, command lists,
unknown context state and future-draw admission. The log does not identify
which mutation subtype ran; resource-aware refusal will still reject any
actual protected write without requiring another diagnostic first. Add
policy and hook-integration regressions that fail this blanket wiring.

The new complete captures cover lower-1152 rows at 3840x2160. Both frames
show zero HDR/depth/stencil changes in that ROI across before/after selected
draw, pre-consumer and pre-Present, despite about 55k selected PS invocations.
There are 146/20 sequences between selected draw and consumer; the F10 trace
again has no events (16-byte header, four skipped slots). This capture does
not prove full-frame suffix safety: upper rows and private coverage are not
captured. The correction therefore depends on explicit resource identity
and owned snapshot semantics, not absence of changes in this ROI. Retain
runtime checks and operation/role refusal diagnostics for remaining writes.

Implemented typed destination roles and resource/view mutation helpers.
Known clears, copies, updates, mip generation, resolves and Map/Unmap
compare their actual destination with every open suffix H/depth; unrelated
resources pass, null/unresolved identities refuse. Scene-b1 uploads still
invalidate camera/projection tracking for future draws but no longer discard
owned snapshots. Engine substitution flush semantics are unchanged. Unknown
command lists and context resets still refuse. Foreign/wrong-thread
mutations latch uncertainty during the suffix; the HDR consumer acquires
that latch before backend evaluation.

Resource failures increment the operation/role counter before the first
failure closes the suffix. The existing 5s report prints and resets those
counters beside marked/isolated/refused counts. Dead instrumentation is
therefore distinguishable from a successful path. Re-read changed runtime,
header, hooks and tests; no declaration/order or duplicate log defect found.
Targeted flat_temporal_test build passes (17 traces, 46/46 replay frames).
New tests exercise unrelated buffer writes through the actual consumer,
protected HDR/depth and unknown refusal, all ten mutation-hook variants,
Map/Unmap and foreign latch wiring, with removal/misrouting mutation controls.
Full validation passed, including GPU rigs and installer gates, with receipt
fingerprint `630ee203b04248f94eeeb2c642db9daac8a6ef285725281e226b5498269c9fd6`.
Committed as `128fca02`, fast-forwarded main and verified the push. Clean
DLL-only promotion passed. Epic flat installed and independently verified
as `v0.18.1-25-g128fca02`; live `edvr-flat.ini` and `nvngx_dlss.dll` hashes
are unchanged. The resource correction is BUILT, NOT FLOWN.

Next representative test: same weapon drawn ten seconds, then holstered
ten seconds. Check that world AA remains active and 5s marked/isolated
counters advance; any relevant mutation refusal now names operation/role.
F10 only if AA still turns off or another defect appears. No two-minute wait
or weapon matrix is required. Verify the next log against the literal
installed `v0.18.1-25-g128fca02`, not the later docs-only HEAD.

## 100. HDR source ambiguity after protected draws (2026-10-03)

Verified `edvr_gfx_20261003_115819.log` against installed
`v0.18.1-25-g128fca02` (6AC141F7), linked 17:57:11 UTC. Sean drew the same
weapon, took F10, waited ten seconds, then holstered ten seconds. At
12:00:35: planned=360, fully-marked=360, isolated=0, refused=360. The sole
layer reason is `overlay-unsealed-at-final-copy`; the earlier HDR consumer
selection is `source-camera-or-depth-not-unique` on all 360 frames. No
resource mutation or old constant-upload refusal appears. The transition
windows each isolate five frames; holstered 12:00:55 treats all 156 frames.
F10 completes frames 46126/46128 with zero unsupported captures.

Ruled out: coarse resource mutation guards still block the protected suffix,
because all those reasons disappear on the verified new build. The final
copy's unsealed refusal is downstream of the HDR source-selector ambiguity,
not the first cause. Identify which source records conflict and why before
editing uniqueness rules. Real second-world source/depth or a mismatched
world camera must still refuse. No new diagnostic flight requested yet.

Source audit: the protected VS025B/PS46F pair is not an engine-supported
Pool pair; only supported Pool draws enter `prefix.sources`. Ruled out:
the protected draw directly adds a competing motion source. HDR ambiguity
can arise from a supported source on H's depth with mismatched DSV/camera,
or an alternate supported depth using H's camera. The manifest retains H
and depth through consumer, with first_bad_seq=0; that does not identify
the offending source record. Five transition frames prove the isolation
route can complete, not that source uniqueness is safe to relax. Keep the
existing two-world-depth and different-camera refusal tests.

The complete VS88DC/PS4945 camera probe is also an unsupported HDR weapon
pass, not a Pool motion source, so neither captured weapon pair supplies the
missing source identity. Existing data cannot distinguish same-depth layout/
DSV mismatch, same-depth camera mismatch, or alternate-depth matching-world
camera. Do not ship a uniqueness exception on this missing evidence.

Implement a bounded automatic witness at the actual HDR selector input:
two Screen records plus at most 32 Pool sources. On two overlay-open
AmbiguousSource frames separated by at least 60 frame indices, report the
H reference and all supported same-extent Pool records, each branch flag,
first culprit, resources/views/formats, VS/PS, copied camera rows, hashes
and sequence/write provenance. No F10 dependency, extra key, temporal-policy
change or prolonged wait. Include an enabled/limit/report summary so a dead
instrument cannot masquerade as successful qualification. Full validation,
commit, clean promotion and Epic install precede the next short run.

The witness is implemented as a synchronous callback from the actual
34-record route wrapper only after AmbiguousSource. It reads live local
record copies before they expire; no dangling camera pointers are retained.
Runtime sampling requires an open marked overlay, reports before spending
the two-frame budget, and exposes enabled/captured/eligible/ambiguous totals
in the 5s log. All eligible source records are enumerated, including records
after the first offender; the first branch respects the selector's two-pass
order. Re-read helper, runtime/report fields and tests; declarations and
format arguments match. Rendering selection and refusal policy are unchanged.

Targeted flat_temporal_test passes (17 traces, 46/46 replay frames), with
wrong-camera, wrong-DSV and true second-world-depth refusal fixtures,
unsupported filtering, selected-frame silence, callback wiring, full fields,
all-record enumeration and sampler/order checks. Full validation passed,
including GPU rigs and installer gates, with receipt fingerprint
`0285c441be41d192768ed3f9ded4e4d65fabf5a3a090cdb542a048ff513160d9`.
Committed as `aca86106`, fast-forwarded main and verified the push. Clean
DLL-only promotion passed. Epic flat installed and independently verified
as `v0.18.1-27-gaca86106`; live `edvr-flat.ini` and `nvngx_dlss.dll` hashes
are unchanged. The automatic diagnostic is BUILT, NOT FLOWN; it changes no
source-selection rule and the remaining AA refusal is still open.

Next short diagnostic: same weapon drawn ten seconds, then holstered ten
seconds. No F10 or two-minute wait is required. The report captures two
failing frames automatically and enumerates their supported source records.
Verify the next log against the literal installed `v0.18.1-27-gaca86106`,
not the later docs-only HEAD.

## 101. Early foreground motion source identified (2026-10-04)

Verified `edvr_gfx_20261004_132943.log` against installed
`v0.18.1-27-gaca86106` (6AC14625), linked 2026-10-03 18:15:01 UTC. Both
automatic witnesses completed, frames 44789 and 44849. First offending
record index 2 is `same-depth-camera`, VS AACFDCF2FB9AD809 / PS
CF534B32F491561A, seven draws, same 3840x2160 Pool color (fmt23), depth,
DSV and b1 as the valid world source and H reference. Layout/current/
viewport/order all pass. Position and orientation rows 4/5 match exactly;
projection scale differs and near is 0.0675 versus world 0.025.

Frame 44789: foreground source seq 4506-4513, key write 4504; valid world
source EB5234DB6ADB491D / CB9F297EFF264251 spans 4529-15056, 9555 draws,
key write 4528; H camera names seq15663. Frame44849 repeats the pattern:
seven foreground draws, then 9596 matching-world draws. This is not a
second depth or DSV mismatch and not stale source provenance.

Ruled out: the selector's culprit is an unrelated second-world-depth source,
because both witnesses show same depth/DSV and identical pose, with only
foreground projection/near different. The exact early supported source is
now known. Do not simply ignore it: shared-depth writes and engine-motion
records must remain consistent with the world resolver's depth convention
and clean history. Audit foreground depth/motion ownership and measured
projection relation before editing admission; no new flight requested yet.

The engine-motion audit confirms first-source ownership is not harmless:
flat naming takes the first supported full-viewport fmt23/26 candidate and
`continuesRun` gates the actual producer by that copied camera. Naming the
early foreground camera can therefore skip all later world MRT6 work. The
established screen-motion path excludes `weaponMotionFamilyVs`, including
the measured AACF source; align flat world naming with this semantic rule.
Keep HDR source ambiguity refusal until foreground ownership is qualified.

The optional VR first-person map/stencil contract cannot yet justify flat
admission. No early draw DS/stencil/arguments are in the witness. A missing
map drops the pair and exposes world-camera fallback; alternate near depth
must not borrow that history. For opaque backends, raw output/rejection alone
also does not prove internal history safety. Late HDR bit0x04 is separate
from first-person bit0x10 and its lower-half capture proves neither early
cohort ownership nor surviving marks.

Implement world-source naming correction plus one bounded diagnostic of the
early cohort: actual producer state, conservative passing-fragment coverage
from the original PS, depth after the cohort and current depth/stencil at HDR
consumer. Count surviving marked/unmarked fragments, overwritten fragments
and marks lacking tracked ownership; include intervening stencil mutations,
world/engine camera identity and every missing-stage status. Max two frames,
automatic, no F10/key. No selector or opaque backend admission relaxation.
This collects the remaining qualification evidence together in one short run.

Implemented the existing first-person-family exclusion for world naming only;
the selector still refuses AACF's alternate projection. The automatic probe
brackets all seven draw thunks, exports private MRT7 coverage from the original
PS without replay, clones shared depth before the first world draw, and compares
the depth/stencil planes at the selector's actual H consumer. Readback is eight
uints, asynchronous, at most two frames separated by 60 frames; combined private
resources are bounded to 128 MiB. Missing stages, predication, active stream
output and unknown work have explicit status rather than a false zero count.

The WARP fixture initially measured zero coverage because its full-screen
triangle was culled. Explicit no-cull state on both original and instrumented
draws corrected the fixture without changing the production counter shader.
The independent 13x9 geometry discards three columns: total 117, covered 90.
Counts then prove 90 surviving unmarked pixels, 90 marked survivors with 27
unowned marks, and after depth overwrite zero survivors with 90 overwritten
pixels. Original color/depth/stencil bytes match the uninstrumented draw.
Full validation passed 2026-10-04 at 14:03 MDT, including both targeted rigs,
shader compilation/reflection, the complete rig suite and installer gates.
Receipt inputs SHA256: `3898af9b73634a9d24ed970faa3a972e68e7669f01e4297b548117ca23c01acf`.
The first full attempt stopped at the old single-diagnostic registry count;
the gate now validates both distinct diagnostics, including ownership CS
8x8x1 and t0-t3/u0/b0 bindings. The complete build was then rerun successfully.
This build has not flown. No AA admission,
configuration, or VR behavior change is included.

Committed and pushed `40c3a1c7` to main. Receipt-guarded `--dll-only` promotion
passed from the clean tree. Installed `v0.18.1-29-g40c3a1c7` to Epic flat at
14:05 MDT using `install_edvr.py`; dry-run wrote nothing, install and
`--verify-only` passed, and the installed DLL ProductVersion is this version.
Backup tag `flat-foreground-40c3a1c7`, stamp `20261004-140552`.
`edvr-flat.ini` SHA256 remains
`1696ADFC572CBA74668C56A60AAD09FDFDD64B1FC947B6759B68925E4EE2DDC1`;
DLSS SHA256 remains
`3975567B8943C53ACCE397F2B72380092F84F162D00B0D2C7D08A1025C563983`.
No INI or DLSS install flag was used. Frontier was not installed.
NOT FLOWN: reproduce with the same weapon for 10 seconds drawn, then
10 seconds holstered, no F10. Verify the literal installed version rather
than documentation HEAD. Read both automatic ownership reports, missing-stage
statuses, named-same-H and engine-source counts; do not interpret zero partial
counts or exact depth alone as exclusive first-person ownership.

### 2026-10-04: world naming confirmed; probe budget spent before reproduction

Verified `edvr_gfx_20261004_140803.log` against `v0.18.1-29-g40c3a1c7`,
build 6AC2B17A, linked 20:05:14 UTC. Sean repeated 10 seconds drawn then
10 seconds holstered; AA still switched off. Source witnesses at frames
44390/44450 both have `named-same-H=1`: corrected world camera, depth and b1
match the actual H reference. Early AACF/CF still has near0.0675 against
world0.025, and source uniqueness still refuses as designed. Late overlay
marking succeeds throughout; refusal clears after holstering.
During the 14:10:19 refusing window the engine wrapper reports 9943.9
substituted draws/frame and GPU resolve zero timed calls, confirming motion
production is active while the mixed-camera selector declines AA.

Ruled out: remaining weapon refusal is caused by the old foreground-first
world naming, because both new witnesses name exactly the H world camera.
Foreground depth/motion/history handling remains unqualified.

Instrumentation failure: loading frames36302/36362 armed at seq64/102,
reported `partial reason=foreground-after-world-source`, draws0, same-phase1,
and consumed both samples before the actual reproduction at frame44390.
All pixel counters are unavailable, not measured zeros. The arming routine
already detects the ineligible already-named-world condition, but records it
as an active failed sample instead of declining to arm. Correct that bounded
eligibility path and test that skipped loading draws leave both samples for
the later early cohort. No AA admission relaxation or new rendering hypothesis
is justified by these partial reports.

Corrected the production arming gate: an idle after-world candidate increments
`late-skipped` and returns before activation, allocation, coverage or reports.
An active cohort extending after world source is explicitly partial, as is an
interleaved shared-depth depth/stencil writer before the snapshot. The selector
and temporal behavior are unchanged. The temporal rig directly replays the
observed loading frames36302/36362, then eligible44390/44450; skipped candidates
preserve both samples, frame44449 is too early, pending/missing-resource/budget
gates remain closed and post-world extension is refused.

Full validation passed 2026-10-04 at 14:19 MDT, all DLL/rig/installer gates.
Receipt inputs SHA256: `fa0ca37638bd2a74c94690811fa70627ed9e911e7f303059c317803eac896883`.
The first attempt stopped at the investigation's 62-line Status block, which
was shortened; the full build was rerun successfully. Arming correction NOT
FLOWN. No INI, DLSS, VR or AA admission change.

Pushed code `2b8f5475` to main. Clean-tree receipt-guarded `--dll-only`
promotion passed. Installed and verified Epic flat `v0.18.1-31-g2b8f5475`
at 14:21 MDT; dry-run wrote nothing. Backup tag `flat-probe-arming-2b8f5475`,
stamp `20261004-142144`. DLL ProductVersion verified; settings and DLSS
hashes match the pre-install snapshot, no INI/DLSS install flags, no Frontier
install. NOT FLOWN. Next repeat10s drawn/10s holstered, no F10; verify the
literal installed version and check late-skipped plus both complete/partial
ownership reports. AA switching off remains expected while admission is closed;
this build repairs evidence collection, not the unresolved foreground path.

### 2026-10-04: arming corrected; early cohort has intervening depth writers

Verified `edvr_gfx_20261004_142246.log`, `v0.18.1-31-g2b8f5475`, build
6AC2B53A, linked20:21:14 UTC. Both samples now arm during the actual weapon
reproduction at14:25:15/16, frames45214/45274; 238778 late loading candidates
were skipped without spending the budget. Both witnesses name exactly the H
world camera (`named-same-H=1`), and early AACF/CF projection near0.0675 still
differs from world0.025. AA refuses while drawn and recovers after holstering.

Frame45214: first5266, last5268, world5274, consumer20616, marked3/planned3;
three intervening same-depth state-enabled depth/stencil writers. Frame45274:
first4957, last4965, world4981, consumer21155, marked3/planned7; fifteen gap
writers. Both report depth-write-all1, stencil-replace16=0, full-viewport1,
same-phase1, no explicit mutations/clears or foreign work. The first latched
`interleaved-shared-depth-draw` failure prevents further marking and dispatch;
all pixel counters are unavailable partial values, not measured zeros.

Ruled out: late loading consumes the probe's budget after the arming fix,
because reported0 survives loading and samples arm only at the reproduction.
Ruled out: the tracked early foreground forms one uninterrupted call sequence
before named world source, because both samples contain intervening draws
bound to shared depth. This makes an after-cohort clone insufficient to
attribute surviving pixels to the tracked draws. The combined state report
also does not certify the VR first-person stencil-replace16 producer contract;
it does not prove that every foreground pixel lacks stencil16 at the consumer.
Audit immediate per-draw passing coverage/depth capture and the omitted writer
states before another flight; retain AA refusal and avoid a per-weapon whitelist.

The later state-query audit qualifies the gap counters: noteSameDepthDraw was
called before the lazy engine-motion flush decision, so a pending substitution
could leave EDVR's OM state bound at the query. These counters therefore do not
certify the next game's depth/stencil operations. Query pre-world gap chronology
after restoring the game's state; do not force a flush across all later world
draws just for diagnostic aggregates. The intervening call sequence and the
probe's sticky failure are confirmed; actual fragment writes remain unmeasured.

The producer audit narrows the finding: `stencil-replace16=0` is ANDed across
the cohort, so at least one marked draw fails the existing VR producer gate;
it does not identify which draw or prove all consumer pixels lack that bit.
`X` is DrawIndexedInstanced; the logged24-index, one-instance draw passes only
the count/instance gate. Flat exits its hook before `weaponMotionDraw`, so
there is no existing producer map to reuse. History safety is required even
for EDVR TAA: current-frame rejection alone does not tag stored foreground
history against a later world pixel with coincident encoded depth. FSR and
DLSS also remain unqualified with shared raw depth and one world near/FOV.

Implement a bounded per-draw measurement: fresh private R8 passing mask for
each eligible original draw, immediate shared-depth copy after that draw,
GPU merge only those passing pixels into persistent union/owner-depth maps.
Intervening draws cannot replace the saved owner-depth values. Compare those
values with actual consumer depth/stencil; retain exact-depth equality's
explicit non-exclusive limitation. Record capped per-draw and pre-world gap
chronology with shader/camera/DS state, and distinguish measured partial counts
from unavailable counts. Coverage-only diagnostic mode skips the unused clean
Pool-color copy, allowing mask1 + union1 + owner-depth4 + depth-copy8 bytes per
pixel (14 B/px, about111 MiB at4K) under the existing128 MiB bound. Max two
frames,60-frame separation, no F10/key; AA selection and temporal history are
unchanged. Tests must execute the canonical merge shader with intervening
depth writes and verify original color/depth/stencil and context restoration.

Sean reports drawing the weapon in VR keeps world AA active. The code paths
explain the visible difference: screen_motion excludes the first-person family
when naming VR world source; vr_world_route has optional weapon motion/stencil
inputs and does not use flat's supported-source camera-uniqueness rejection.
The flat hook exits before the VR weapon producer. Corrected flat world naming
therefore restores engine production but not the separate HDR admission guard.
This is a reported VR observation, not a new VR log qualification; no VR install
or VR behavior is changed in the per-draw diagnostic.

Per-draw implementation audit: each eligible draw uses a fresh passing mask,
runs the original draw once, restores its layer state, copies depth immediately
and merges under saved/restored context state. Only pre-world gaps on the exact
shared depth resource force lazy-state restoration before querying game state;
later draws only increment a resource-identity counter, explicitly unobserved
for DS state. Capped chronology includes both faces' stencil functions and all
operations. Predication/stream output and read-only depth have named refusals;
failed/subset captures cannot appear as complete measurements. No AA admission
or backend history policy changed.

Targeted WARP test: two real raster draws reuse the same coverage-only layer in
one frame, with an intervening depth write. The second mask is 25 pixels (105
would expose a missed clear), persistent union 105, final surviving 25 and
overwritten 80. Overlap takes the second draw's depth; untouched pixels retain
the first draw's depth. Original color/depth/stencil match the uninstrumented
baseline, and no clean HDR texture/view is allocated. Shader reflection retains
the merge's 8x8 group, t0/t1, u0/u1 and b0 slots. Targeted mono and temporal rigs,
generator self-test and runtime syntax compilation passed.

Full absolute-path build passed all gates, including production DLLs, Python
self-tests, GPU/test rigs and self-contained installer resource checks. Receipt
input fingerprint: `ae977748c6961e0af59443086295929dff7a2c9800313771fb9665c7a6525886`.
The full gate preceded clean-commit promotion and Epic installation below.

Install completed: code commit `2657c1f4` fast-forwarded to main and pushed;
clean receipt-guarded DLL-only promotion passed, then the sanctioned installer
dry-run, install and verify-only all passed for Epic's flat package. Live DLL
ProductVersion/FileVersion: `v0.18.1-33-g2657c1f4`. Flat INI and DLSS SHA256
match the pre-install snapshot exactly; Frontier/VR was not installed.
INSTALLED, NOT FLOWN. The AA refusal remains during this measurement. Next:
the same weapon drawn for 10 seconds, then holstered for 10 seconds, no F10.
Read measured versus unavailable counts, every eligible draw's merge status
and the intervening chronology before changing AA admission.

### 2026-10-04: per-draw measurements obtained; stencil16 is incomplete

Verified `edvr_gfx_20261004_145317.log`, `v0.18.1-33-g2657c1f4`, build
`6AC2BC1C`, linked 20:50:36 UTC. Samples at 14:55:26/27, frames45724/45784,
both report `complete counts=measured`: planned/drawn/merged 1/1/1, one F event,
no G events or chronology overflow. First coverage0; second coverage1020,
surviving-exact-depth1020, surviving-marked16=0, surviving-unmarked1020,
overwritten0. Second consumer has497800 stencil16 pixels elsewhere. These are
measurements of the nominated draw, not proof that all weapon pixels were
captured. The 24-index AACF/CF draw has near0.0675, while the named H/world
camera uses0.025. HDR witnesses identify exactly this one mismatching Pool
record and1402/1449 matching world-source draws, `named-same-H=1`.

Both F events: writable depth, GREATER_EQUAL, stencil ALWAYS/REPLACE on both
faces, ref5, write-mask21 (bits0/2/4). This deliberately clears bit16 while
writing the low bits; a generic foreground classifier cannot assume every
first-person contribution uses the VR producer's stencil16 convention.

Ruled out: the sampled second draw's final depth differs from its immediate
captured depth, because all1020 covered pixels compare exactly and overwritten
is0. Equal bits still do not prove exclusive final ownership; later writers
may write identical depth. The first sample's zero coverage does not explain
why that original draw had no passing samples. No same-depth pre-world gaps
were observed; after-world1966/2009 same-DS draws have unobserved DS state.
`foreign=1` with `reason=none` needs a boundary audit: active persists through
asynchronous readback and `noteForeign` currently accepts post-consumer work.

AA timeline: 14:55:23 selects/treats379/379; 14:55:33 refuses350/350 for
`source-camera-or-depth-not-unique`; 14:55:38 refuses183/treats163; 14:55:43
treats366/366 with zero late-overlay draws after holstering. Late overlays are
fully marked but remain unsealed during rejection. Code seals only inside an
admitted HDR treatment, so the later `overlay-unsealed-at-final-copy` can be a
consequence of the earlier source conflict, not an independent uncovered draw.
Audit final contributor ownership and temporal history before changing that
admission. No additional flight requested from this result yet.

Read-only code audit resolves both reporting questions. `noteForeign` accepts
work after `consumerSeen_` while readback keeps `active_` true; consumer checks
would have latched `foreign-work-during-probe` if foreign work preceded the
measurement. Thus these reports' `foreign=1 reason=none` is post-consumer
pollution, not evidence against the measured counts. Nomination is faithful:
the independent HDR witness has exactly the one eligible AACF/CF record in
each frame, so planned1 is not lost capture. The late overlay refusal counts
139/350/183 match source ambiguity; the selector refuses before treatment,
which is the only place that seals the protected HDR suffix. No independent
uncovered late writer is evidenced here.

The engine slot/depth map is not a foreground ownership channel. The flat
producer runs only for world `continuesRun` candidates, excluding AACF, and
the prep shader takes the world-camera term when a slot is absent. Extending
stencil16 or ignoring the AACF record would therefore misclassify its pixels.
Implementation boundary: final per-pixel world/foreground/unknown provenance
must follow every relevant original writer in order, with unknown writers and
explicit mutations refused. A prior-frame domain must prevent world history
from inheriting foreground at coincident depth. FSR/DLSS additionally need a
qualified depth/projection and history contract; one current-frame mask does
not establish that. No further diagnostic-only flight is required to repeat
the source conflict, measure the same subset or investigate the derivative
seal refusal. The next validation flight should exercise implemented generic
handling rather than another speculative marker heuristic. AA fix still open;
no C++ or installed-package change from this flight analysis.

## 102. Preserve world AA with conservative camera-domain exclusion (2026-10-04)

Sean authorized implementation after the measured section101 flight. The
required property is a trusted subset of world pixels, not an exclusive final
owner for every pixel. A persistent R8 MRT7 mask unions passing fragments of
every qualified alternate-camera contributor. Later world overdraw may retain
a mark: that sacrifices AA locally but cannot assign world motion/history to
foreground. The original shader still draws once with its depth/stencil and
color behavior. Clear once per frame; no per-draw full-size copy or dispatch.

The consumer must prove all conflicting source records were captured, with
current camera/phase/pose, shared depth, native render/output extent and valid
HDR lineage. Unknown writers, capture/state failure, mutation or a missing
record preserve refusal. Existing late HDR isolation is retained. Its suffix
forbids depth writes, so the clean color snapshot and final depth plane still
match; no additional depth copy is necessary.

SDK history has no proven per-pixel camera-domain reset. Modern NGX presets
ignore the available bias-current-color mask (`dlaa.cpp`); AMD documents FSR
reactivity as reducing history influence rather than removing it. Qualified
mixed-camera frames therefore use EDVR TAA internally, with foreground current
color. Single-camera frames retain the configured backend, with history reset
on transition. No INI key, setting change, per-weapon table or VR change.

TAA checks all four current bilinear taps against the untrusted union and
writes current color/domain0 when any tap is untrusted. Valid world output gets
domain1 even if this frame cannot reuse history for another reason. All four
previous bilinear taps must be domain1 before history is sampled, in addition
to existing depth/motion/rejection checks. The current 3x3 clamp omits untrusted
neighbors. Output-domain ping-pong follows color/depth history and is reset on
entry/exit, resize, failure and mode transition; even a configured TAA mode
must reset on coverage-presence changes. Null coverage preserves existing
never-mixed and VR arithmetic. Foreground uses no world near/FOV or motion, so
its raw depth need not be converted for this current-color-only treatment.

Environment: Epic flat, native3840x2160 from the verified section101 flight;
no VR runtime/headset dependency. Initial admission requires native-size HDR.
Tests must exercise the actual capture and TAA shaders: original rendering
preserved, stencil16 cleared, equal-depth world overdraw remains conservatively
marked, every current/previous bilinear tap checked, coincident-depth history
refused, world history retained outside coverage, late overlay preserved and
entry/exit reset exactly once. Targeted validation passes: generated shader
contracts, resolver syntax, the WARP resolver rig and the temporal policy rig.
The production capture test retains its 105-pixel union across an equal-depth
world pass and a second captured draw, with original color/depth/stencil bytes
preserved. Missing records, unknown mutation and the 128-draw cap fail closed.
Both same-mode history transitions reset once, then world history resumes.
The absolute-path full build completed with exit 0 and all gates passed.
Receipt input fingerprint:
`a316677aaf53f275b41c228c4d01e961d2b3cd48508705d4f5036644cf69dd56`.
Code commit `b38eff130a6146cf79420f2a22c4ebe151897845` is pushed to main.
The clean, receipt-guarded DLL-only promotion passed; Epic flat installation
and a separate `--verify-only` passed. Installed DLL file/product version is
`v0.18.1-36-gb38eff13`. The pre-install `edvr-flat.ini` and `nvngx_dlss.dll`
SHA-256 values are unchanged. Frontier/VR installation was not changed.

INSTALLED, NOT FLOWN. Next: restart Epic, use the same native-size scene and
weapon, hold it drawn 10 seconds and holstered 10 seconds; no F10 needed.
World AA should remain active while drawn. Automatic coverage summaries must
show selected and actually-treated mixed frames with effective TAA, and the
configured backend should return after holstering. A selection without actual
treatment or a capture refusal is distinguishable in those summaries. This
tests the runtime integration; the GPU rigs do not claim a successful flight.

### 2026-10-04: capture admission refused before TAA

Sean flew the installed version and reported world AA still turns off.
Verified log `edvr_gfx_20261004_153808.log`: `v0.18.1-36-gb38eff13`, build
`6AC2C633`, linked 21:33:39 UTC. From frame41394 at 15:40:07.969, coverage
reports draws0/ready0/unknown1, `untrusted-alternate-unqualified`. Every mixed
coverage summary has selected0/actually-treated0. The fallback never ran.

Ruled out: bad domain-TAA accumulation as this flight's cause, because no frame
reached that path; capture admission failed first. The guard combines
supported pair, full viewport and current camera checks, so its reason alone
does not identify the failed condition. Source witnesses at frames42347/42407
show the same early AACFDCF2FB9AD809/CF534B32F491561A pair current1/viewport1,
layout1, matching world depth, with three/seven draws respectively. Investigate
nomination scope and guard call order before weakening any ownership check.

The existing camera validator accepts writeSeq <= drawSeq; passing sequence+1
is not an equality failure. The runtime can nominate unsupported same-depth
draws whose camera bytes differ from the first named source. The failure also
appears while holstered AA is treated, so the failed candidate need not be the
early measured weapon source. The current log cannot distinguish:

- unsupported nominee: supported0, with its VS/PS and naming stage;
- camera freshness/hash failure: current0, write epoch/sequence/hash evidence;
- viewport failure: viewport0 with actual extent/range;
- earlier sticky failure: valid nominee, plan refused with prior failure;
- unclassified consumer: captured union, unknown same-depth source differs
  from the authoritative HDR camera, or the bounded source table overflowed.

Collect all five together in bounded automatic logs before changing production
admission. No shader whitelist, settings change, forced F10 or per-weapon run.

The bounded diagnostic is implemented: 32 ordinary distinct nominee signatures,
one reserved valid-but-sticky report per pre-world/post-name role, 16 consumer
source signatures and a first-overflow identity. Reports execute independently
of successful capture, include camera-shape/phase comparisons and actual draw
sequence, and summaries expose witness/drop counts. No capture policy changes.
The absolute-path full build passed all gates with exit 0; receipt fingerprint
`ff4fe85eee85f373000d844bc120e9a471a8052b1f96e9eb35544eb244b42d19`.
Diagnostic code commit `b32db53e92bd6dd46d25cef08189f50ba8bfd77d` is pushed
to main. Clean, receipt-guarded DLL-only promotion passed, followed by Epic flat
installation and separate `--verify-only`. File/product version is
`v0.18.1-38-gb32db53e`; `edvr-flat.ini` and `nvngx_dlss.dll` hashes are
unchanged. The diagnostic is INSTALLED, NOT FLOWN, with v36 rendering behavior.
Next: same weapon drawn 10 seconds, holstered 10 seconds, no F10. AA may still
turn off; the report must identify the failing nominee/check and any valid
draw blocked by it before production admission changes.

### 2026-10-04: shader-support gate is the measured blocker

Verified `edvr_gfx_20261004_155725.log`: `v0.18.1-38-gb32db53e`, build
`6AC2CB1B`, linked 21:54:35 UTC. Sean still observes world AA turning off.
Frame44358 q1631 nominates VS7B0DC42D383F694C/PS0DF03E64DF9DBEF1 before world
naming: supported0, viewport1, current1, near0.0675. This sets the first sticky
`untrusted-alternate-unqualified` failure. The consumer identifies an earlier
unclassified CFCA8FFC6B058630/8A08FF781272C5F6 draw at q1630 with the same
depth/camera, phase-pair1, near0.0675 versus authoritative world near0.025.

At frame45026 q4530, 8B589D25B2A0ADDC/7268762D11A610F2 again reports
supported0/viewport1/current1 before naming. The following AACFDCF2FB9AD809/
CF534B32F491561A draw at q4531 reports all three checks1, prior-failure1 and
planned0. Coverage and mixed TAA never run; periodic selected/treated stay0.

Ruled out: stale camera or viewport mismatch as this capture failure, because
both predicates are1 on the first failing nominees. The measured blocker is
the world-motion supported-pair gate and its sticky effect on later valid
draws. Adding those shader hashes is not a general solution: the first
unclassified source even uses a VS outside the pre-world family nomination.
Coverage must include every alternate-camera contributor and exclude the
authoritative world domain, independently of the world-motion recipe list.
Original passing-fragment coverage can veto history without reconstructing
foreground motion. Establish that complete boundary before implementation.

Implementation boundary: two bounded camera/color/depth/DSV buckets capture
structurally eligible native-size format23 draws before world naming, without
the motion-pair or VS-family list. After naming, capture same-depth camera
mismatches. At the authoritative HDR consumer discard world-camera buckets,
require exactly one complete alternate bucket and verify its current pose/phase
and every observed unsupported draw as well as the supported source records.
An unsupported-only alternate bucket must also select domain-aware TAA. More
domains, missing capture, relevant patch/state failure, mutation or overflow
refuse. A world bucket's failure can be ignored only when it matches the
authoritative world camera; its pixels never enter the alternate mask.

Two R8 planes cost about16.6MB at Epic native3840x2160, reused across frames;
one alternate SRV is borrowed, with no mask-combine pass or per-draw full-size
copies. Generic original-PS MRT7 export remains the actual patch/binding gate.
The regression must call the runtime's shared nomination decision, then replay
unknown CFCA, unsupported material variants, AACF and world selection. It must
exclude world coverage, refuse a missing variant and test unsupported-only
mixed treatment. Holstering need not return the SDK if first-person material
draws still form a real alternate-camera domain; single-camera frames do.

### 2026-10-04: complete camera-domain capture implemented

The shared runtime nomination rule now includes unsupported material pairs
and unclassified geometry. Only the classifier's proven projection-independent
VS InertNoCB plus PS Clean combination is exempt. The first world-naming draw
is excluded because EDVR substitutes its motion shader; earlier world-camera
coverage is discarded at H. Each relevant alternate draw needs a completed
original-fragment receipt, including unsupported-only camera domains.

Certification now reads the supported record's frozen camera bytes. Its key's
camera pointer was only a presence marker pointing into a later-changing draw
scope. A regression mutates that producer scope after freezing the record.

Targeted mono WARP and temporal policy regressions pass: actual shared
nomination, unsupported variants followed by AACF, world bucket exclusion,
original color/depth/stencil preservation, persistent coverage through equal-
depth world overdraw, missing receipts, unsupported-only treatment and domain
overflow. All four exact Epic v38 pixel shader blobs and their production MRT7
patches also pass WARP CreatePixelShader; no game bytecode is tracked.

CFCA's actual vertex shader projects geometry through cb0 rather than the b1
lens. Coverage is conservative; a new occupancy report distinguishes a local
foreground veto from a mask covering too much of the scene. After a successful
mixed resolve, one unsupported-only and one supported-alternate sample may
copy the R8 mask to staging. Later Presents poll with DO_NOT_WAIT, with explicit
unavailable/timeout reporting. This diagnostic never changes admission and
uses no per-weapon hashes. Log prefix: `flat untrusted mask occupancy:`.

Full absolute-path `build.bat --jobs 4` passed all gates, including GPU rigs,
quiet serial reruns and installer-resource validation. Receipt input hash:
`be79e2d20ba260224f36d70db4c90b273c6a97e65e58123d9c915c0906531bd9`.
Log: `build/flat-complete-camera-domain-full.log`. Clean-version promotion and
Epic installation are next. INI, DLSS and VR behavior remain untouched.

Commit `129cbb13` merged to main, pushed and confirmed at `origin/main`.
Receipt-guarded absolute-path `build.bat --dll-only` passed; production DLLs
carry `v0.18.1-40-g129cbb13`. Epic flat installation passed its dry run,
installation and separate `--verify-only`. The installed product version
matches; `edvr-flat.ini` and `nvngx_dlss.dll` SHA256 hashes match the fresh
pre-install snapshot. No `edvr.ini` was present. Frontier was not installed.

INSTALLED, NOT FLOWN. Next: restart Epic, same weapon drawn 10 seconds then
holstered 10 seconds, no F10. Expected: world AA continues through the draw.
Inspect selected/actually-treated and mask-occupancy measured fraction. If
treatment still refuses, the completed/observed counts and capture failure
identify the remaining boundary. If treatment runs but AA looks absent, mask
occupancy tests conservative overcoverage in the same flight. The configured
SDK resumes on single-camera frames; holstering alone need not remove every
alternate first-person material draw.

### 2026-10-04: v40 still refuses; independent Astra review

Sean reports world AA still turns off when the weapon is drawn. Verified
`edvr_gfx_20261004_162843.log`: `v0.18.1-40-g129cbb13`, build `6AC2D263`,
linked 22:25:39 UTC. At 16:30:50.427 frame44845, the CFCA/8A08 alternate
scene source reports observed4/completed0, matching H depth and valid camera
shape/phase. Coverage reports draws0/ready0/unknown1/mixed0, selector
`source-camera-or-depth-not-unique`, failure `untrusted-source-identity`.

At 16:30:59.247 frame45463, the AACF/CF supported alternate record has six
draws q4610..4615; the world EB5234/CB9F record has 9485 draws. Subsequent
coverage summaries at frames45600/45900 remain selected0/actually-treated0,
last-draws1/ready0/unknown0, failure `untrusted-source-identity`. There are no
mask-occupancy entries: successful mixed treatment, which queues those
samples, was never reached.

Ruled out: removing the motion-pair nomination gate alone is sufficient,
because v40 still fails original-fragment capture's source-identity guard.
Mask overcoverage and TAA accumulation remain untested; this run fails before
either executes. An independent Astra agent is reviewing the exact predicate,
runtime ordering, resource state and whether the synthetic rigs cover them.
No new fix, install or test flight is requested during that review.

Independent Astra review completed. The first blocker is proven at
16:30:50.412 frame44845 q1593: PS=0, valid viewport/current identity, camera
`262106EADACB94DA`. H in the same frame has exactly those world-camera bytes.
`FlatUntrustedCoverage::plan` rejects `!ps` as a global source-identity failure
before assigning a bucket; `alternateBucket` rejects that global flag before
it can discard the exact world bucket. Alternate CFCA/7B draws then report
prior-failure1/planned0. This violates the implemented failure-scope boundary.

Frame44967 q1820 demonstrates the intended behavior for a non-null shader:
F516/B40B world draw plans, the next nominee reports `no colour output`, and
that patch failure is bucket-local. A following null-PS draw q1823 upgrades
the failure to global. The existing regression instead captured the alternate
first, then failed a nonzero-hash world bucket with admissible=false; it did
not exercise the observed shader guard or ordering and runtime source counts.

The weapon rendering transition is around 16:30:59 frame45458. Runtime
treatment stays at1340 through 16:31:09 and resumes by 16:31:14, consistent
with Sean's drawn/holstered test. Input actions themselves are not logged.
Mixed selected/actually-treated remain0 throughout; mask overcoverage and
domain-aware TAA remain unqualified in the game.

Recommended correction: keep identifiable shader failures in their known
resource/camera bucket, retain observation counts, and discard the failure
only after exact H world-camera matching. Preserve global failures for
unassignable identities and overflow. Do not blanket-skip null PS or zero-
color-output draws: they may still change depth/stencil; alternate-domain
uncaptured effects must continue refusing. Keep the generic patcher unchanged.

Required regressions use shared production nomination and accounting: world
null PS first, unsupported alternate variants, AACF, H; real zero-output PS
followed by null PS; either shader failure in the alternate domain still
refuses; a nominal world bucket not matching H still refuses; missing receipt
still refuses; original color/depth/stencil remain intact. Then one same-
weapon flight measures treatment and existing occupancy together. Astra made
no source edits, build or installation; the installed v40 remains current.

Sean authorized implementation after the review. Correction scope: known
camera/resource shader failures remain local to that bucket; unassignable
identity and capacity failures remain global. Regress the measured ordering
through the shared runtime nomination and observation-accounting code, then
full build, clean commit promotion and Epic flat installation. The installed
v40 remains current until those gates pass; no extra flight is requested yet.

Production correction implemented: `plan` first validates assignable identity,
then finds/creates the resource/camera bucket. Null VS/PS is now a local
`untrusted-source-shader-identity` failure. Exact H world-camera exclusion
precedes inspection of that bucket's failure; alternate failures remain
ineligible. Global identity and capacity refusal and the patcher are unchanged.
Runtime observation aggregation and completed-receipt comparison now call the
same helpers used by the regression rather than a copied counting model.
Astra's final production/regression audit found no blocker. Targeted mono
WARP and temporal-policy rigs pass. New tests exercise the actual shared
nomination, observation aggregation and receipt comparison, including the
null-PS world-first order and a real no-color-output world shader followed
by null PS. Both preserve the full 105-pixel alternate union and original
color/depth/stencil. Alternate shader failures, H camera mismatch and an
actual unbracketed alternate draw refuse; that missing draw reports observed5
versus completed4. Readback size checks prevent empty-equals-empty false
passes. Sources frozen; full build is the next gate.

Full absolute-path `build.bat --jobs 4` passed all gates, including complete
GPU/policy rigs, quiet serial reruns and installer-resource validation.
Receipt input hash:
`b436a93920cf67991456283f9f84afbac91793cef968d883640c6ea96af42112`.
Log: `build/flat-bucket-local-failure-full.log`. Commit, clean-version DLL
promotion and Epic flat installation are next; v40 is still installed.

Commit `8afc3e01` merged to main, pushed and confirmed at `origin/main`.
Receipt-guarded absolute-path `build.bat --dll-only` passed. Epic flat
installation passed dry run, installation and separate `--verify-only`;
installed product version is `v0.18.1-43-g8afc3e01`. Fresh pre-install SHA256
comparison confirms `edvr-flat.ini` and `nvngx_dlss.dll` unchanged. No
`edvr.ini` was present; Frontier was not installed.

INSTALLED, NOT FLOWN. Next: restart Epic, same weapon drawn 10 seconds and
holstered 10 seconds, no F10. Expected: H discards the exact world-camera
shader failures, selected/actually-treated become nonzero, and mixed TAA
keeps world AA active. The existing mask-occupancy report will measure the
alternate union after successful treatment. Remaining visual/coverage
uncertainty is explicitly unqualified until this flight. Verify the next log
against the literal installed version above; the installation-journal commit
does not change DLL behavior or require another promotion.

### 2026-10-04: v43 admits unsupported-only capture; weapon still refuses

Sean reports AA still disengages with the weapon drawn. Verified
`edvr_gfx_20261004_165257.log`: `v0.18.1-43-g8afc3e01`, build `6AC2D869`,
linked 22:51:21 UTC. Frame44050 q1806/1807 CFCA/7B now plan despite an earlier
world-null-PS bucket failure. H reports observed4/completed4, ready1, unknown0,
mixed1, failure none. Summaries frames44400/44700 select and actually treat300
frames with no refusals. Unsupported-only mask sample frame44052 measures
marked0/8294400; this is a completed zero readback, not an absent instrument.

At 16:55:08.560 frame44945 q4788 (8B/7268) and q4789 (AACF/CF) both plan0.
The supported AACF witness has five draws q4789..4793; frame45005 has seven
draws q4973..4981, both current/viewport/order1 and same H depth/DSV/extent.
Summaries frames45300/45600 show selected0/actually-treated0, refused300,
last-draws1/ready0/unknown0, selector source-camera-or-depth-not-unique and
last-failure none. Runtime treatment stops at1256 during 16:55:12/17 and
resumes after 16:55:22, consistent with drawing/holstering; input not logged.
The terminal late-HDR-overlay/depth conflict is downstream of source refusal.

Ruled out: v40's global world-null-PS poisoning persists, because v43 captures
all four alternate draws and treats before the weapon appears. Weapon mask
overcoverage is not measured: that supported-alternate path never treats.

Astra traced a reporting gap: nonzero/admissible weapon draws with plan0 and
no surviving global failure imply a pre-existing local failure in their
identified camera bucket. Pre-consumer failure() returns the first bucket's
failure, which can instead be the world's `no colour output`; qualifies()
discards the alternateBucket reason, H then skips select(), and consumer()
hides unselected local failures. Thus the first failing alternate draw and
predicate are not identifiable from this log. Actual PS binding of the later
8B/AACF draws cannot explain their refusal: their beginDraw is never reached.
Review existing dumps first; if insufficient, instrument first failure origin,
qualification checks and actual patch/state failures together in one run.

Astra completed the evidence review: existing creation-time shader dumps do
not recover the first failure's dynamic camera/order. No trace dump or armed
projection audit occurs in this flight, and the old foreground probe reports
active0/reported0 throughout the drawn interval. A rendering fix is therefore
not yet justified. An early alternate null-PS/depth-only draw is plausible,
not proven.

The next build is diagnostic only: retain each bucket's first failure origin
and all record-qualification predicates, actual PS identity on begin failures,
and report world/alternate roles against H even when selection fails. Reserve
two supported-alternate refusal reports (second at least60 frames later),
separate from holstered success; retain reasons after consumer completion and
count emitted/dropped diagnostics. Tests must prove distinct failed-bucket
provenance, refusal-report emission and reason survival. No admission change,
settings key, per-weapon table or GPU wait. One run measures the remaining
competing boundaries together; do not silently bypass depth-only effects.

Sean asks whether F10 can speed diagnosis and explain the problem fully.
Capture audit: the v43 F10 draw/pixel path only finalizes two qualified
successfully treated frames, with sparse pixel windows and stride/quota
sampling; failed weapon frames are discarded. Its independent always-on trace
does retain observed draw keys and frozen camera bytes, but only the last
three completed frames fit, each capped at4096 events; truncated frames are
omitted. Neither path retains private MRT7 capture failure/receipt details.
Thus existing F10 can complement the diagnostic, not recover its missing
first failure reliably. Do not increase trace quotas or rewrite capture
admission in this change. The bounded failure report also requests cached
creation bytecode of the first failed nominee and actual PS, so one combined
run supplies the data for offline reproduction rather than another guessed
rendering fix. Next capture: draw weapon, press F10 once, wait15s, holster10s.

Combined diagnostic implemented and independently reviewed by Astra with no
admission changes or blockers. Targeted mono WARP and temporal-policy rigs
pass: two distinct failed buckets retain the alternate reason after consumer,
supported refusal reporting reaches H, original outputs match, and the sample
budget survives holstered success and enforces the60-frame interval. Actual
PS object identity is separate from fingerprint, so an untracked shader is
not reported absent. Failure-only binding queries see original/restored state.
Bounded emitted reports also retain raw frozen H, bucket and qualification-
record rows (maximum384 bytes), with record first/last sequences; the updated
WARP checks assert these snapshots match owned camera bytes. No GPU work was
added for those rows. Sources frozen; full gate next.

The first full gate passed (receipt2489b11d), but the final data audit found
one material gap before installation: shader hash0 means absent OR untracked
nonnull PS (`lookupShaderHash` and the binding hook explicitly handle both).
The original diagnostic sampled physical PS only at begin failure, so a plan
shader-identity failure could still require another flight. It now snapshots
physical PS only on that bucket's first matching shader-identity plan failure,
under InternalScope after the existing restore/plan path; sticky and valid
plans add no queries. A regression distinguishes null from nonnull unknown
hash and proves repeated attempts cannot overwrite the first snapshot.
No admission change. Updated sources frozen; full gate rerun required.

Final full gate passed: `build/flat-first-failure-diagnostic-full-final.log`,
exit0, all gates passed; receipt `1be12b21ebe757920894f400e81bd7899c4c825617ea87c0351118ef03e8a131`.
Astra approved the final plan-failure PS snapshot and its regression, with
no blocker or admission change. This receipt supersedes the first full gate;
commit, clean DLL promotion and verified Epic flat install next.

Diagnostic code `0f20f4e9` merged to main and pushed. Clean receipt-guarded
promotion passed (`build/flat-first-failure-diagnostic-promotion.log`); Epic
flat install and separate `--verify-only` passed. Installed ProductVersion
`v0.18.1-45-g0f20f4e9`. SHA256 confirms live `edvr-flat.ini` and
`nvngx_dlss.dll` unchanged. No Frontier install. NOT FLOWN; this build records
the failure without changing AA admission. Restart Epic, draw the same weapon,
press F10 once, wait 15s, holster for 10s, then exit. Read the next flight
against this literal installed version after the documentation-only commit.

### 2026-10-04: v45 identifies the first weapon-camera failure

Verified `edvr_gfx_20261004_172835.log`, installed
`v0.18.1-45-g0f20f4e9`, build `6AC2E095`, linked 23:26:13 UTC. At
frame42878 q4923 and frame42938 q5187, the first alternate bucket failure is
VS `F516BF0201303B87` / PS `B40B0462256E31C2`, begin `no colour output`.
Actual PS matches the nominee; current, viewport, H DSV, shape and phase all
pass, global failure none. Observed15/37 draws have completed0; supported
AACF qualification has matching resources/camera but ready0 and receipts0/5
or0/7. Thus the first failed capture prevents later weapon receipts.

Ruled out: the weapon's first failed draw has a null, untracked or mismatched
PS, because the physical nonnull PS is known and exactly matches B40B.
Saved PS disassembly proves an empty output signature, optional cb2-controlled
texture alpha test and `discard_z`, followed by a terminal `ret`; no explicit
depth, coverage, stencil or UAV output. The existing patcher rejects empty
signatures before it can append its private coverage export. A general pure
empty-output extension can retain those instructions and alpha/depth effects;
skipping the draw would lose the depth ownership evidence. Offline regression
must compare original color/depth/stencil and private surviving-fragment mask,
including alpha enable/disable and subsequent supported-camera qualification.

Astra independently confirmed B40B and that F516 projects via cb1[270..273].
Compiled SV_Target7 and Elite's color signatures use OSGN systemValue0;
retain that representation for empty signatures. The new empty-output case
must refuse forced early depth/stencil (opcode106 flag0x2000): otherwise
discarded fragments could write depth before reaching the coverage export.
B40B has only refactoringAllowed. Existing colored-shader admission is outside
this correction's scope; explicit depth/coverage/stencil/UAV guards remain.

F10 at17:31:00 armed frame42954. Its trace dump has frames0/events0,
skipped-slots4. Draw pixels retry unqualified weapon frames42955..43314,
then save partial frames43369/43370 at17:31:15 under
`flat_draw_pixels/20261004_233100_014_60168_1`, with copy/cap refusals.
The independent first-failure reports, saved exact shader and camera bytes
are sufficient for this correction despite the absent whole-frame replay.
Summary frame43200 treats0/refuses300; frame43500 treats134/refuses166;
frame43800 treats300/refuses0. Recovery is consistent with holstering;
the input event itself is not logged. Next flight should test actual coverage
and treatment after the offline regression, not add another instrument.

Generic correction implemented: empty original OSGN can gain private MRT7,
with original instructions unchanged; forced early depth/stencil is rejected
only for this new case after both chunks are parsed. Shader creation caches
the patch; no new per-draw query or setting. Astra reviewed production and
implemented WARP regressions using the exact 384-byte B40B shader, whose
EDVR hash is asserted. Alpha off/mixed/all-pass/all-discard, depth and stencil
rejection with stencil writes, exact original color/full depth-stencil bytes
and restored bindings pass. Empty alternate then unsupported material then
AACF has complete receipts and H qualification; equal-depth world overdraw
retains conservative coverage. Forced early and reversed chunk order,
explicit depth/coverage, UAV and unsupported control flow refusal pass.
Old no-output refusal fixtures now use explicitly unsupported SV_Depth.
Targeted absolute mono build exit0; sources frozen and final full gate running
(`build/flat-empty-output-full.log`). NOT FLOWN; admission is structural,
with no weapon identity or shader hash list.

Final full build passed all gates, exit0, source tree frozen:
`build/flat-empty-output-full.log`; receipt
`d7bad299440972c707bfc3dfcd8336091061369a32438424b4d84513ba63411f`.
Commit, clean receipt-guarded promotion, main push and Epic flat install next.

Fix code `ab74d281` merged to main and pushed. Clean DLL promotion passed,
exit0 (`build/flat-empty-output-promotion.log`); sanctioned Epic flat install
and separate verification passed. Installed version `v0.18.1-47-gab74d281`.
Live flat INI and DLSS SHA256 match pre-install snapshots; Frontier untouched.
NOT FLOWN. Restart Epic; hold the same weapon drawn for 15s, holster for 10s,
then exit. If world AA still disengages, take F10 while drawn; otherwise the
automatic capture/coverage and treatment counters suffice. Match the next
flight to this literal installed version after the documentation-only commit.

### 2026-10-04: v47 captures the depth pass, then refuses a resource mutation

Sean reports world AA still disengages with weapon drawn; F10 taken.
Verified `edvr_gfx_20261004_174952.log`, `v0.18.1-47-gab74d281`,
build `6AC2E57E`, linked 23:47:10 UTC. Frame44439 alternate F516/B40B has
pending1/completed1/ready1; frame44499 has pending13/completed13/ready1.
H camera/DSV/extent/phase and qualification predicates all pass. First failure
is now `mutation/untrusted-source-explicit-mutation`, with global failure none;
observed9/completed1 and observed37/completed13, AACF receipts0/3 and0/7.

Ruled out: accepting B40B's empty signature alone restores weapon AA, because
the capture now succeeds but a later explicit mutation prevents later draws.
The B40B correction itself is flight-qualified. The first-q field records the
last nominee (q4612/q4776), not the actual mutation's operation or sequence.
Do not infer a depth clear or stencil-only clear from that field.

ClearRtv, ClearDsv (including stencil-only), copies and other resource writes
all collapse into noteMutation(resource). A stencil-only clear is plausible;
genuine depth replacement requires a different response. Inspect existing
footprint/capture evidence before editing admission. F10 frame44548 trace is
frames0/events0/skipped-slots4; draw pixels retry failed frames, then capture
partial holstered frames44933/44934 under
`flat_draw_pixels/20261004_235204_520_4644_1`. No whole failing-frame replay.
Treatment frame44700 is38/refused262, frame45000 is70/refused230, frame45300
is300/refused0; recovery matches the holster sequence, whose input is not logged.

The saved weapon-footprint frames44549/44551 identify the same depth/DSV and
report no clears after their late selected draw. That tracker starts too late
to exclude an earlier clear and does not cover every depth mutation. Dispatch
does not call this coverage bucket's noteMutation; it cannot directly explain
the measured explicit-mutation failure. No rendering-policy change is justified
by these captures.

The next build is diagnostic only. Preserve mutation entry point, operation,
resource role, owned API payload, actual after-q position, notification ordinal
and capture state through sticky refusal. Preserve the last nominee separately
from the mutation position. Retain sixteen exact-payload signatures per bucket
with repeats/drops, plus independent first occurrence/count for each operation
and role so many variants cannot hide a later mutation type. Observe later
nominee shader preparation after refusal without creating GPU shaders or
changing admission. Continue existing budgeted H reports on refused frames.

Increase the four-slot F10 ring from4096 to65536 events per frame (about126MiB
bounded CPU storage, allocated on the heap). Keep existing V3/V4 binary layouts;
report attempted events, overflow slots and other skip reasons explicitly.
Tests must cover a twenty-thousand-event refused-frame round trip, exact cap
and overflow, and mutation payload/state lifetime across repeats, later writes,
consumer, reset and signature exhaustion. No per-weapon table or settings edit.

Astra reviewed the frozen hook-to-report path, metadata ownership, registry
lock scope and independent operation/role coverage: rendering admission is
unchanged. Globally unassignable draws still follow existing early refusal;
registry and bytecode success do not prove later device creation or full draw
state eligibility. These remain explicitly labelled limitations.

Full absolute build.bat --jobs4 exited0, with mono resolve and temporal
collector rigs PASS, including the new mutation and busy-frame cases; existing
corpus replay is46/46 frames identical. Installer resources verified and all
gates passed. Fresh full-pass receipt created2026-10-05T00:22:26.193917Z,
inputs SHA256 `0ce1cffb78b6b9e78fb7d773b22d835f492b67cf33b6457fad4a9d8f0fd7d6de`.
An earlier direct run_jobs invocation failed its missing parent build environment
before either affected rig ran; it supplied no test evidence. Validation came
from the full build, with no source changes during it. Diagnostic NOT FLOWN.

Validated source committed as `5698007f`, fast-forwarded to main and pushed;
origin/main confirmed that exact commit. Clean --dll-only promotion verified
the full-build receipt and exited0. Sanctioned Epic --profile flat dry-run,
real install and --verify-only all exited0; installed DLL ProductVersion is
`v0.18.1-49-g5698007f`. Receipt backup
`edvr_flat_receipt.json.pre-flat-mutation-context-5698007f-20261004-182850.bak`.
Root repeated verification: flat payload/profile match, exact SHA256 of live
edvr-flat.ini and nvngx_dlss.dll unchanged, edvr.ini remains absent. Frontier
not installed. Bind the next log to this literal version after doc-only commit.

Next flight: restart Epic, draw the same weapon, press F10 while AA is refused,
hold10s, holster10s, then exit. No two-minute wait or per-weapon testing required.
This build changes evidence collection, so world AA may still refuse. Use the
actual mutation operation/flags/payload and later nominee reports to select
the rendering fix; do not infer it from the preceding draw or an empty trace.

### 2026-10-04: v49 identifies the pool color clear and retains the full frame

Sean completed the short F10 sequence. Verified edvr_gfx_20261004_183816.log,
`v0.18.1-49-g5698007f`, build6AC2EF2D, linked2026-10-05 00:28:29UTC.
Failed frames54886/54946 have completed9/9 and13/13 alternate captures, ready1,
then ClearRenderTargetView on shared pool color0x1F094368AA0, values all zero,
after-q4610/4710. Generic flatRuntimeWritten duplicates the notification at the
same q. Each bucket has two detailed signatures/two classes, no drops; global
failure none. H camera/depth/DSV, shape/phase and frozen-camera predicates pass.

Ruled out: depth or stencil clear causes these measured refusals, because both
complete mutation reports identify only a color ClearRtv and its generic write.
Five later/earlier nominee pairs all have saved VS/PS bytes, patchable bytecode
and eligible registry metadata. Three patched PS objects exist already; two
are not yet created. No later bytecode/registry blocker appears in this capture;
that still does not prove every later GPU/state admission.

F10 frame54972 retains three complete frames,61219 events,30854536 bytes;
peak25041/cap65536, overflow0, one in-flight slot skipped. Existing trace-check
parses all three and replays contracts identically. Actual V4 events are504B
(include32B HDR SRV identities), so fixed ring is132120792B, about126MiB; the
earlier472B/118MiB estimate omitted that V4 field and is corrected above.

Astra's decoded frame54969 shows depth-write marker q4517, alternate B40B
prepass q4519..4531, world prepass, pool color-write q4677, then alternate and
world material draws using the same depth/DSV and two frozen cameras. No depth
write/copy marker occurs after the alternate capture begins. H is a different
resource0x1F09436BC20, unique, with late-writes0. This is deferred-pipeline
ordering and supports retaining the private coverage union across the color
clear while keeping independent prefix/HDR write accounting. Before editing,
review duplicate generic-notification handling and WARP preservation/refusal
tests; no broad exemption for copies, updates or unknown writes.

The correction retains coverage only for typed ClearRtv on bucket.color when
that resource differs from bucket.depth. The ClearRtv hook labels its generic
flatRuntimeWritten callback with the same operation; every other caller defaults
to Written. Do not infer provenance from sequence adjacency. Diagnostic mutation
records remain. Prefix, HDR, camera, projection and overlay write observers still
run in their existing order; HDR color clears remain protected independently.

GPU regressions execute real zero/nonzero pool clears between original draws:
prepass marks survive, later marks accumulate, world overdraw cannot erase them,
H qualifies with complete receipts, and color/full depth-stencil bytes match the
uninstrumented baseline with restored bindings. Separate later-unregistered PS,
untyped write, wrong-role ClearRtv, depth/stencil clear, copy, update, resolve,
null and foreign controls still refuse. Tests also patch and create the five
actual v49 PS blobs on WARP. Four new fixtures plus existing CF534 were verified
by Astra against their EDVR FNV hashes and sizes: B40B:384B,8A08:216B,7268:7104B,
CF534:3456B,0DF0:7208B. Source/test independent review approved; full build pending.

First full validation compiled production, then the enlarged mono GPU fixture
exited0xC00000FD (stack overflow) before assertions. The five new large capture
objects were moved to scoped heap ownership, with explicit memory include;
assertions and production policy unchanged. Astra reviewed this correction.
Full rerun uses pool-color-clear-full-rerun.log, retaining the failed build log.

Full absolute build.bat --jobs4 rerun exited0: mono resolve and temporal
collector PASS, all five real PS patches created on WARP, installer resources
match and all gates passed. Fresh full-pass receipt created
2026-10-05T01:05:20.064909Z, inputs SHA256
`a7e7550b5ebe65f0b0104da3f229e61b2283d9e0159338a66ace921ead890bf6`.
No production edits followed independent review; heap-only test repair retained
every assertion. Color-clear correction BUILT, NOT FLOWN.

Validated source committed as `30ced747`, fast-forwarded main and pushed;
origin/main confirmed that exact commit. Clean --dll-only promotion verified
the full-build receipt and exited0. Sanctioned Epic flat dry-run, real install
and --verify-only exited0. Installed ProductVersion `v0.18.1-51-g30ced747`,
receipt backup `edvr_flat_receipt.json.pre-pool-color-clear-30ced747-20261004-191121.bak`.
Root repeated payload/profile/version verification and exact baseline SHA256
checks: edvr-flat.ini and nvngx_dlss.dll unchanged, edvr.ini absent. No Frontier
install. Bind next flight to this literal version after documentation commit.

Confirmation flight: restart Epic, draw the same weapon, F10 while drawn,10s
drawn and10s holstered, then exit. Check that world AA stays engaged and that
post-clear alternate captures/receipts complete through H. If still refused,
use the preserved first-failure and later nominee diagnostics; do not assume
the color-clear policy failed. No two-minute wait or per-weapon checklist.

### 2026-10-04: v51 confirmation across several weapons

Sean reports the fix worked, then tested several weapons and world AA stayed
engaged. Verified edvr_gfx_20261004_200720.log against literal installed
`v0.18.1-51-g30ced747`, build6AC2F925, linked2026-10-05 01:11:01UTC.
Frames52200..56400 sampled windows report selected300/actually-treated300,
capture-refused0, ready1, failure none and qualification qualified. Alternate
draw counts change through4,34,28,32,37 while treatment continues. The previous
all-refused weapon interval is absent. Later windows return to the ordinary
world path (world-excluded300, effective DLSS), not a coverage refusal.

In the mixed-camera windows configured DLSS uses effective TAA; this confirms
AA engagement, not DLSS execution in those windows or a performance gain.
No F10/H audit is present in this flight, and inputs/weapon names are not logged;
associate the multi-weapon result with Sean's report rather than inventing a
per-weapon timeline. Together with v49's explicit clear and complete trace,
the unchanged safety checks and WARP regressions, this flight qualifies the
scoped color-clear fix. Close the weapon/world-AA disengagement investigation;
no further weapon-by-weapon test required. Other rendering/runtime arcs remain
open as listed in Status. Keep diagnostics available for a future distinct fault.

## 103. 2026-10-05: plasma-weapon late-overlay refusal on v0.18.2

The reporter says Aphelion laser-rifle AA works, while switching to a plasma
shotgun or plasma pistol stops it. Sean confirmed both supplied ZIPs come
from this one user. The longer `edvr_logs.zip` export continues the same
`edvr_gfx_20261005_080621.log` as `edvr-logs-20261005-081245.zip`, ending
08:14:18.258 rather than 08:12:42.574. Sanctioned log-tool checks verify
v0.18.2, build6AC30AC5, linked2026-10-05 02:26:13UTC. Four older v0.18.1 logs
in the larger archive are historical. The profile is flat, RTX 3050,
1920x1080, DLSS model K. Host OS/runtime is not identified; a system32 path
does not establish Wine/Proton. Logs contain no weapon labels.

At frame18420, 08:09:36.859, `flat runtime conflict` reports
`unprotected-late-hdr-overlay-or-depth-write`, ref-q1089/current-q0. The empty
current witness is also produced by `overlayFail`, so it does not prove an
actual resource write. The corresponding H identity is absent and the two
buckets become `unrelated-depth`; qualification was not called. Both buckets
share depth/DSV but have different camera IDs. A null-PS plan failure exists
in one bucket, but cannot explain the earlier H selector failure by itself.

Periodic `flat late overlay refusal` reports `dual-source-blend`: 288 at
08:09:43.480, 123 at 08:09:53.505, and 300 in the sustained 08:10:54.612 and
08:11:04-14 windows. Frame23100 reports selected0/capture-refused300; other
windows treat300/300. The guard in FlatOverlayLayer::validateBlend rejects
SRC1 factors before private-MRT capture; overlayFail then invalidates H.
This explains the synthetic conflict and is the primary remaining refusal
category. The exact live blend descriptor and offending shader are absent
from current diagnostics, so active dual-source versus dormant fields is
not yet established for this reporter.

Ruled out: repeating the shared-color-clear hypothesis, because the recorded
color ClearRtv and runtime-written duplicate are tolerated by section 102;
the sustained refusal counters identify the separate blend guard.

Ruled out: using F10 trace14759 as the failing plasma sequence, because its
three frames14756-14758 replay with produced1/copy-selected1, one H, zero
ambiguity/late writes and one resolve marker each. Its 08:08:31 dump predates
the sustained refusals. Trace14719 is empty. The archive has60 DXBC files;
only blobs tied to current log identities establish current shader provenance.

Ruled out on WARP: ordinary inactive SRC1 fields surviving GetDesc and
causing production rejection. A temporary MSVC probe called the actual guard
and WARP CreateBlendState/GetDesc: disabled-blend and independent-false
dormant descriptors are API-valid but normalized to ordinary factors before
the guard reads them. Active RT0 SRC1 survives and is rejected. Enabled SRC1
in RT1/RT7 is invalid even with a zero write mask. RT0 zero-write or
noncontributing-channel edges remain possible; their effective behavior is
not a license to attach an extra MRT during active dual-source blending.

Next: diagnostic only, preserving capture/AA behavior. Record the actual
GetDesc result, active RTVs, all blend factors/enables/write masks, and shader
identity/output metadata at the refused draw, publish alongside the existing
five-second refusal report, and distinguish no sample from successful capture.
Sean will test one affected plasma weapon on Epic. No per-weapon matrix or
two-minute wait is needed. Do not relax the guard before this evidence.

### Diagnostic implementation and local reproduction

Sean also reports the symptom with any plasma weapon tested locally. The
latest Epic baseline log, edvr_gfx_20261005_045207.log, verifies as installed
v0.18.1-51-g30ced747/build6AC2F925. Its paired late-overlay/runtime counts
255/412/43 at05:04:18/23/28 and258/392/38 at05:04:58..05:05:08 report the
same dual-source-blend and HDR-conflict reasons as the supporter. Frame100363
has the same synthetic empty-current conflict, at3840x2160. No F10 trace
dump is present; automatic projection capture saved74 shader stages/37 pairs.

The diagnostic adds an optional data snapshot only on the existing
dual-source guard failure. The runtime annotates its live VS/PS and draw
sequence before overlayFail replaces the conflict witness. The existing
five-second reporter emits guard-failures, sample status, one sample and all
eight target descriptors, then rearms. An armed/ready startup line and
samples0/no-dual-source-sample window distinguish no event from missing code.
Original PS output signature is explicitly unknown in this path; hashes
identify bytecode for later analysis rather than inferring output signatures
from a patched shader. No admission, binding, shader or AA behavior changes.

Focused WARP tests exercise the existing refusal with actual GetDesc data,
first-sample retention/reset, ordinary acceptance without a sample, and
byte-identical color/depth/stencil plus unchanged OM/PS bindings. Pure
classification tests cover independent targets, unbound slots, disabled
blending, contributing write channels and MIN/MAX operations. Full validation
passed: 117 parallel jobs and five quiet jobs, mono resolve PASS, config
contract234/234, both installer resource checks and all final gates. Receipt
fingerprint11609745426ba8bd3305e8344465bc1360b045121e2a7aa86dadebc825c9cd95
matches the validated source. Commit/promotion/install follow this gate.

Validated source committed as a2e406c5, fast-forwarded main and pushed;
origin/main confirmed the exact commit. Clean --dll-only promotion verified
the receipt and passed. Sanctioned exact-path Epic flat dry-run, install and
verify-only passed; root repeated verification. ProductVersion is
`v0.18.2-1-ga2e406c5`. Backup receipt:
`edvr_flat_receipt.json.pre-plasma-overlay-a2e406c5-20261005-052320.bak`.
Flat settings and DLSS SHA256 match the pre-install baseline; edvr.ini stays
absent. Frontier was not installed.

Next flight: launch Epic, draw one plasma weapon, and once world AA disengages
wait five seconds before F10. Keep it drawn another five seconds, then exit.
Read against literal installed v0.18.2-1-ga2e406c5 after this doc commit.
Check armed/ready, samples1/captured, the actual blend sample/target rows and
paired refusal counts. samples0/no-dual-source-sample is a distinct result,
not evidence that a fix worked. This is measurement only; guard behavior
still refuses the original unsupported case. No two-minute wait or per-weapon
matrix required.

### 2026-10-05: diagnostic flight and replay proof

The completed plasma test captured an active RT0 dual-source blend: `SRC1_COLOR`
and `SRC1_ALPHA` are enabled on the active target; RT0 mask is `0F`, effective
SRC1 channels `0F`, and slots 1–7 are inactive. The same failed shader pair is
in all three complete F10 frames. Depth-write is off; the trace stencil-write
flag is enabled, but its flag/mask do not prove an actual stencil operation
wrote. The original VS/PS DXBC is absent, so PS outputs and side effects remain
unknown. See `build\supporter-0182\diagnostic-flight\evidence.md` for sample,
frame, descriptor, hash, and refusal-window details.

Ruled out: inactive/unused SRC1 false rejection, because active RT0's actual
GetDesc uses SRC1 color and alpha. Ruled out: unconditional forced-early UAV
capture, because the temporary UAV proof changes discard/query counts; stencil
semantics remain unknown. The private-DSV replay proof passes with production-
style dual-output shaders, preserves color and depth/stencil (including
discard), and preserves occlusion queries and pipeline statistics when no
external query is active. Its negative proof shows an external active query
counts replay twice, so production use requires query guards (and SO guards).
Results: `build\supporter-0182\dual_source_replay_probe_results.txt` and
`build\supporter-0182\dual_source_uav_probe_results.txt`.

The production-DXBC proof now passes all four variants: plain, discard,
derivative/discard, and original early-depth semantics. Original color,
depth/stencil and query results match the untouched draw; coverage excludes
failed depth/stencil tests and discarded fragments. The original shader's
early-depth flag is preserved. The extra pass requires an inactive count-
bearing query bracket; the negative proof detects double counting otherwise.

The implementation derives a private RT0 coverage shader lazily from the
existing validated MRT7 bytecode. Original outputs become private temporary
registers, retaining the original calculations and discard. Before each
eligible dual-source draw, it copies the pre-draw DSV to a reusable private
DSV, executes coverage there, restores the game bindings, and leaves the
original dual-source draw unchanged. Clean HDR is copied before the first
overlay; subsequent coverage accumulates without refreshing that copy.
Normal MRT7 capture is unchanged. No weapon identities or new config keys.

Admission retains the late-overlay depth/stencil ownership rules and refuses
predication, stream output, graphics UAVs, indirect/DrawAuto, uncertain state,
and active count-bearing queries. Query observation starts with the first
flat-profile game Begin/End, independently of temporal mode and Present, and
survives resize/ClearState. Context/query identities are retained. Existing
128 MiB HDR/mask limits remain; a separate 64 MiB private DSV limit supports
3840x2160 D32S8 (63.28 MiB). This is desktop D3D11 capture; VR uses its existing
path. WARP validation has no debug layer installed on this machine.

Focused mono/GPU validation passes, including real layer color/depth/stencil
equivalence, discard and derivative coverage, original early-depth behavior,
mask union, query-model refusals, and invalid-bytecode negatives. The final
absolute-path full build passed after source freeze: production DLLs, all
117 parallel and five quiet jobs, 234/234 config contract, and actual installer
resources. Receipt fingerprint:
`56faf69b59ca4adce7ebb48c01486c328e0d6c7a7a50479a1030975f561cc47b`;
log: `build\plasma-overlay-replay-full-final.log`. Epic qualification remains
open; perform the clean receipt-guarded promotion before installing. The five-second
`flat late overlay replay` report distinguishes candidates, completed original
draws and specific refusals; one plasma weapon is sufficient for this test.
Keep section 102's color-clear fix qualified.

**2026-10-05, Epic qualification of f13ee92b:** clean promotion and verified
flat install completed with the INI and DLSS preserved. The flight
`edvr_gfx_20261005_063607.log` matches `v0.18.2-3-gf13ee92b` (build 6AC39824).
World AA still disengages with plasma drawn. Ruled out: the deployed replay
fallback qualifies this plasma shader, because every replay window has zero
completed draws and `replay-PS-bytecode-not-retained` (234, 349, 223, 134, 73
candidates). The private-DSV mechanism has not run on the affected draw.
Live VS/PS bindings are present: 7F9B650EC1A1E570 / CBB1A87D6023B2A8. Their
original DXBC files are absent. F10 `flat_trace_45355.bin` records three frames,
60,687 events and no overflow; staged evidence is
`build\supporter-0182\replay-flight\evidence.md`. Retrieve the creation-time
qualifier reason and original shader bytes before changing shader admission.
The generic replay currently hides the original cache exclusion reason.

Raw creation bytes are nevertheless present in the separate flat probe cache:
at 06:38:22.963 the exact pair is `verdict=generic-recipe`, `ps=clean`,
`ps-reason=none`. `classifyGenericPair` reads both raw cache entries; a missing
PS returns NoBytecode, never clean. Ruled out: the single raw-cache drop or
one-MiB blob cap explains this pair, because its raw PS classified clean.
Absent files are an export-trigger gap. Next diagnostic preserves the actual
overlay exclusion reason and exports the original pair once on replay refusal.

**2026-10-05, capture the draw before another flight:** Sean requested full
draw evidence comparable to the VR NumLock diagnostic and NumLock as the flat
default. The existing point chronology excludes non-depth-writing/non-motion
draws, then discards unqualified HDR frames. Those gates exclude the failed
plasma overlay. The new packet capture is independent of that qualification,
prioritizes observed refusal pairs, and retains original and executed pipeline
state plus draw inputs and before/after outputs. Shader admission is unchanged.
Limits and missing data must be explicit in each packet and its capture-level
manifest; no packet with omitted required data may claim completeness.
The budget is one GiB per arm, sixteen priority pairs with two distinct frames
per pair, and four representative categories capped at 256 MiB so they cannot
consume the priority reserve. Files split into at most 64-MiB chunks. This is a manually armed
desktop D3D11 instrument; it adds no per-draw copies while unarmed and does not
change the VR runtime. The log bundler needs a streaming ZIP writer before
its aggregate capture limit can safely increase. NumLock uses the existing
`hotkey.dump_draws` action in flat; the VR keys and capture behavior remain.
Epic's explicit F10 binding will be changed to NUMLOCK with its other settings
preserved. Capture implementation is validated; do not qualify the
plasma rendering fix from offline capture tests.

**Capture validation:** the focused WARP rig passed with a nonempty original
dual-source/discard/stencil-04 draw. Captured before/after color and depth/stencil
match the original draw; separate original/executed mutable constants, shader
bytes, textures, high-slot geometry and nonindexed start arguments are retained.
Each selected frame/q joins its sealed same-frame trace. The offline reader
validated the genuine complete fixture and seven intentional partial fixtures.
Hidden SO offsets, UAV counters, predicate/query results and AA-Off resource
mutation chronology remain explicitly incomplete. Binary output reserves
64 MiB of the one-GiB limit for metadata. The streaming ZIP tests passed CRC and
extraction, full-session folder selection, atomic cap omissions, ZIP32 bounds
and failed-output cleanup. The full validation build passed; no new flight
has qualified the plasma rendering fix.

The first full gate stopped on unchanged heartbeat stress check `H5.whole`.
Its combined sample-count/record-consistency predicate does not identify which
condition failed. The unchanged executable then passed all 30 focused checks;
the capture sources are not linked into that rig. A full `--jobs 8` rerun keeps
all checks enabled and reduces concurrent scheduler pressure. This is not
evidence of a proven heartbeat root cause.

The `--jobs 8` full gate passed: 117 parallel jobs, five quiet jobs, config
contract 234/234, and actual installer resources. Receipt fingerprint:
`df10048331b953d69c820aa80115ab1f57cc6258492604ac9c935a684901d875`.
Two older temporal source pins were updated for the explicitly armed Paused
capture exception and the separate AA-Off request consumer; focused checks
retain the unarmed Paused freeze and all 46 identical frame-contract replays.
Next promotion installs the clean DLL on Epic and migrates only its explicit
F10 binding/help comment to NumLock. Plasma shader admission remains unchanged.

2026-10-05, NumLock flight: `edvr_gfx_20261005_090010.log` matches
`v0.18.2-4-g85329199`, build `6AC3B4FD`. At 09:03:06.769, frame 50147,
draw q=17202 reports `replay-PS-bytecode-not-retained: unsupported PS control
flow or declaration`. The original VS (4948 bytes) and PS (66988 bytes) are
available and saved; the raw PS preflight independently gives the same
refusal. Ruled out: missing original shader bytecode, because this diagnostic
exports it successfully. The next discrimination is the actual rejected
opcode: balanced loops/switches may preserve the terminal coverage mark;
calls or nonterminal returns require a different proof. Do not broaden the
qualifier until the saved program establishes which case is present.

Saved PS `CBB1A87D6023B2A8` establishes that case: the first rejected token
is LOOP (opcode 48), SHEX word 755, disassembly instruction 86. The shader
contains nine balanced LOOP/ENDLOOP pairs, nine BREAKC, fifteen CONTINUE,
and one BREAK; its seventy IF/ENDIF pairs are balanced, maximum typed nesting
eight. It has one top-level terminal RET (instruction 2287), no RETC, calls,
switches, discard or UAV operations. Read-only raw SRV loads were already
admitted. Hypothesis confirmed: rejecting structured loops prevents the
qualified shader from entering the replay registry. A typed IF/LOOP stack
can prove that local loop branches cannot bypass the final coverage mark;
the repair must retain the original instructions and reject crossed blocks,
unmatched branches and nonterminal returns. Test the exact saved shader's
normal/replay creation and dynamic-loop coverage with discard offline.

NumLock capture `capture_1109472078_48932` saved two plasma packets,
frames/draws 49882/17170 and 49883/24332. The validator correctly refused
their trace association: the matching frame and shader pair occur once,
but the normal runtime trace recorded `key.sequence=0`. Packet metadata uses
the frame reducer's actual draw sequence. The capture-only Off/Paused path
sets the trace sequence; the normal path omitted it. Repair only the trace
copy after the reducer advances, preserving the reducer's original
observation and cache behavior. Existing bytecode and draw snapshots remain
usable; do not label the old event traces complete or relax validation.

The first packet's original draw state also satisfies the existing private
replay guard: one RT0 with enabled ONE/SRC1_COLOR (alpha ONE/SRC1_ALPHA),
full color mask, effective depth write disabled, stencil mask/reference
`0x04`, no SO/OM-UAV bindings and no active counting query. Its output and
DSV snapshots are present. This confirms a shader-qualification repair can
reach the existing generic replay path without weakening those guards.
The second packet hit resource limits; its payload cannot substitute for
the first packet's complete resource snapshots. Raw shader evidence is
staged under `build\numlock-capture-evidence`.

The focused O2 mono rig passed (`build\packet_loop_focus.log`): the exact
66988-byte captured PS qualifies and both derived variants create on WARP.
Dynamic nested loops with continue/break/discard preserve original color,
depth and stencil; private RT0 replay uses the pre-original cloned DSV and
matches normal MRT7 coverage for stencil-fail, pass and depth-fail cases.
The patch preserves every original instruction byte. Crossed/unclosed
blocks, loop-less branches, malformed controls and nonterminal/conditional
returns remain refused. The trace regression now uses the real reducer,
ring serialization and parser across a camera event and a frame reset,
asserting emitted frame/draw/hash association with unchanged observations.
Production validation is in progress; no flight has qualified this repair.

The first full gate stopped on two older temporal source-wiring pins after
the trace-copy call changed. A deeper review found a real regression in
that proposed trace copy: contract hashing includes `key.sequence`, while
the live reducer retains the original zero and offline replay would consume
the trace's numbered key. Ruled out: numbering `key.sequence` is an isolated
trace repair, because a produced frame's contract hash would change on
replay. Keep the original key and use an explicit `EDVRFTR5` draw-number
field in the event's former padding at byte 468; size stays 504. Legacy
FTR3/FTR4 parsers retain their original keys and ignore the new field. The
regression must replay a produced frame with original sequence zero, verify
the contract hash and independently join its packet draw number. Do not
install the first full build (`build\plasma-loop-full-build.log`).

FTR5 is implemented with the separate draw number. Both focused O2 rigs
passed (`build\packet_loop_focus.log`, `build\packet_temporal_focus.log`):
live-like zero-key-sequence produced contracts replay to the same hash,
and all 46 historical frames across 17 corpus traces remain identical.
Legacy alignment padding is explicitly ignored. Regenerated complete
packets validate and satisfy `--require-complete`; intentionally partial
cap/timeout/Off/overflow/map-failure cases return the expected 0/2.
The reader also refuses a changed trace shader with otherwise consistent
packet metadata. The final full build is running in
`build\plasma-loop-full-final.log`; Epic confirmation is still pending.

Final full validation passed: production DLLs, 117 parallel jobs, five quiet
jobs and the self-contained installer gates. Receipt input fingerprint:
`8b6bc74178416ecdabdd0641016b509ad03b5b4bc1c3b8b8dc231acbdfa33246`.
The source is ready for commit, main push and clean DLL-only promotion.
Install on Epic with the current flat INI preserved. Next flight: draw the
same plasma weapon; if AA drops, press NumLock and keep it drawn ten seconds.
No two-minute wait and no weapon-by-weapon test matrix are required.

2026-10-05, follow-up flight `edvr_gfx_20261005_095515.log` matches installed
`v0.18.2-5-g7bc87490`, build `6AC3C7A3`, linked 15:52:03 UTC. Sean still
sees AA disabled with plasma and took NumLock. Ruled out: the loop gate alone
explains the visible fault, because at 09:58:16.835 all 76 replay candidates
complete with zero refusals, and the same plasma PS is retained/eligible.
The log reports 3274 treated frames overall; temporal draw failure counters
are sparse. NumLock selected tone/resolve draws on frames 45215-45217.
Next discriminate backend disengagement versus an overbroad overlay mask:
check actual resolve eligibility/backend evaluations, and measure the saved
coverage texture's nonzero fraction and extent. The captured plasma PS has
no discard, so surviving raster fragments are not proof of visible color
contribution under dual-source blending. Require actual mask/output evidence
before changing coverage semantics; retain the qualified loop repair.

The later focused log read confirms 1348 replay candidates/completions and
zero replay refusals. At 09:58:11.835, 141/141 HDR frames reach, evaluate,
finish and restore; 141 continue valid history, with cumulative backend
failures zero. Ruled out: backend disengagement or permanent history reset
explains this steady interval. `configured=dlss effective-last=taa` is a
separate confirmed policy override: `treatHdr` chooses TAA for mixed cameras,
and the renderer currently refuses untrusted coverage for SDK modes.
Sean explicitly requires honoring configured DLAA/FSR without that downgrade.
Support the configured backend safely; do not merely remove the selection
ternary and leave the renderer's SDK-input refusal in place.

All six new FTR5 packets validate, but omit the now-successful plasma pair
and the private EDVR coverage textures. The actual t12 overlay mask is absent,
and the HDR consumer's large resource payloads were capped. A genuine earlier
supported-alternate untrusted-mask GPU sample (frame 44417) marks 5465 of
8294400 pixels (0.066%); this refutes a full-screen untrusted union for that
sample, not the unmeasured plasma overlay mask. Prefer offline footprint
replay of the older exact plasma packet: its shader has no discard, depth
or sample-mask output, so the captured VS/IA/depth/stencil path plus a solid
PS can establish raster extent without all material textures. Color-change
claims additionally require the original material shader and HDR parity.

The older original plasma packet `49882/17170` has every required original
resource (468732820 bytes). The new offline WARP diagnostic recreates its
original shader, IA, raster, blend and depth/stencil state. Passing raster
coverage is 84181/8294400 pixels (1.01491%), with inclusive bounds
`(2351,1535)-(2657,1899)`. Captured before/after HDR changes exactly those
84181 pixels; no captured masked pixel is bitwise identical. WARP depth and
stencil match the captured after image exactly. Color parity is incomplete:
13098 pixels differ, all within coverage, maximum decoded channel error 0.5,
maximum scaled error `abs(delta)/(1+abs(captured))=0.027027`; 18 pixels exceed
1% scaled error, none are nonfinite. Do not attribute the differences to
hardware without evidence, or treat this as exact material-output parity.
Ruled out: this saved plasma draw's own passing raster mask covers the entire
world, because it covers only 1.01491%. Other overlay draws, their union, and
the current flight remain unmeasured. The legacy trace association remains
invalid; original resource/state replay does not repair or validate it.

Mixed-camera coverage is logged at frame 43232, 784 frames before the plasma
pair's first logged late-overlay draw at 44016. The forced TAA choice therefore
predates that plasma draw and is a separate policy issue. Runtime selection,
preflight and telemetry must preserve `s.engine`. Until SDK input ownership is
qualified, an explicit refusal is honest; it is not an AA fix. Do not install
a downgrade-removal-only change and claim the plasma problem is solved.

The private pixel capture cancels TAA/FSR routes and only runs after successful
backend evaluation. That explains the missing private masks in this NumLock
capture. Add manually armed prebackend evidence before the mixed-camera SDK
guard: original H/depth, optional clean H, full-plane overlay/untrusted masks,
camera/phase/backend metadata and explicit prebackend status. Stage resources
asynchronously without GPU waits, retain no borrowed game pointers, and keep
the byte cap. A refused attempt must still export its available inputs; it
must not fabricate successful backend images. The existing successful pixel
capture needs mode-correct TAA ping-pong output if extended to TAA/FSR.

Implemented diagnostic, 2026-10-05: all three flat route selectors now retain
the configured engine. The SDK mixed-camera refusal guard is unchanged:
DLAA/DLSS/FSR may decline these frames, but cannot silently run TAA. This is
not a qualified plasma-AA fix. NumLock stages prebackend H, raw depth,
optional clean H, full overlay/untrusted masks, slots, the structured pool
and scene-now/previous buffers, with camera, phase and prep metadata. Present
polls owned staging resources without GPU waits; absent inputs, budget caps
and failures are explicit. Two attempts share a 384 MiB cap, with 64 MiB
file chunks. The log bundler includes each input-capture directory atomically
for the selected session. The older successful-output capture still covers
DLSS/DLAA only; these input files never claim a backend output.

Focused validation: actual WARP renderer calls for DLAA/DLSS/FSR retain the
SDK refusal, leave H unchanged and complete the input capture through normal
Present polling. Exact mask/engine bytes, CRCs, view ranges, all-mips views,
unarmed inertness, budget/timeout/publication failures and whole-session
bundling pass. Existing 46-frame temporal corpus remains byte-identical.
The standalone offline replay has missing/corrupt-input and write-free dry-run
checks plus real depth/stencil pass/fail cases and private-DSV isolation.

Environment: Epic flat desktop DX11, 3840 x 2160 render/output; no VR runtime
or headset in this test. Installed NVIDIA DLL file/product version is
`310,9,1,0`, SHA256 `3975567B8943C53ACCE397F2B72380092F84F162D00B0D2C7D08A1025C563983`.
The internal two-camera coverage limit is unchanged. Next evidence is one
plasma-drawn NumLock capture and 10 seconds for asynchronous readback: compare
the full overlay/untrusted union with the captured engine records and camera
inputs. No weapon matrix or two-minute wait is required.

Full validation: `configured-backend-inputs-full-retry.log` passes all 122
jobs, quiet checks and installer/package gates. Receipt fingerprint is
`7836d2f8919896989d15ee47c0276648e52d791da7c181200be2e20b61aa9bdf`.
The first `--jobs 8` run stopped at the unchanged heartbeat H5.reader check,
which combines throughput and tear detection without distinguishing them.
Two isolated runs passed all 30 checks; the full `--jobs 4` retry passed the
unchanged gate. No heartbeat source or assertion was changed. The coverage
summary now labels the configured attempt `attempt-mode`, rather than
claiming an effective backend on refused frames. Runtime refusal only
invalidates history; Present still polls the staged input capture.

### 2026-10-05: TAA works; configured SDK route still blocked

Flight `edvr_gfx_20261005_114905.log` matches installed commit `f2e7c6c7`,
version `v0.18.2-7-gf2e7c6c7`, build `6AC3E232`. Sean reports TAA works
with plasma. The log records 147/147 TAA HDR treatments in a steady window;
sustained DLSS later reaches the resolver but has zero prep/backend calls,
with `flat-resolve-untrusted-coverage-requires-native-HDR-TAA`. This is the
remaining input-ownership guard, not a weapon replay rejection or SDK failure.
The second NumLock is labelled DLSS rather than DLAA. A separate DLAA warning
is a 512 x 512 render versus 3840 x 2160 output mismatch, and FSR initializes
only briefly without a recorded completed resolve. Do not claim a measured
sustained DLAA/FSR backend failure from this log.

Both prebackend dumps validate all nine payloads/CRCs, 220826112 bytes each.
The first TAA sample has empty coverage masks and clean/current HDR equality;
it does not establish the mask state of every plasma frame. The second DLSS
sample has 13.2571% overlay and 12.0363% untrusted coverage, union 21.9532%.
Clean/current differ at 26028 pixels, all within the overlay; 721292 untrusted
pixels outside the overlay have identical clean/current colors. The late-H
snapshot therefore does not certify removal of earlier foreign-camera
Gbuffer contributions. Both dumps have reset/zero-phase current-equals-prior
camera metadata, so they cannot establish animated history correctness.
Evidence: `build/configured-backend-flight/{evidence.md,findings.txt,comparison.json}`.

Ruled out: whole-frame coverage causes this sustained SDK AA loss, because
the captured union is localized and the resolver refuses before evaluation.
Ruled out: repeating these bulk dumps supplies previous animated foreground
positions, because they omit foreign prior B1, original VS/IA and paired bones.
VR already retains post-VS positions and geometry/skeleton identities. Flat
needs exact final Gbuffer/pixel ownership plus its own projection/depth and
raster phase qualification before that shared motion can feed SDKs. Stencil16
is insufficient: the measured material pass explicitly clears it. Do not
restore secret TAA, remove the guard, zero foreground motion, reset all history
each frame or treat reactive masks as strict RGB-history exclusion.

Sean reports NumLock tanks FPS. Four weapon-footprint samples alone record
1260519424 bytes (about 1.17 GiB), in addition to the two 211 MiB input sets.
There are no >=250 ms freeze lines; missing timing counters prevent exact
attribution of the frame-rate loss. Source review finds synchronous packing,
CRC and filesystem export on Present, despite nonblocking GPU maps. Another
broad capture is not needed for this issue. Sean approved ordinary NumLock as
a bounded general shader/state/census/routing report, with bulk pixel/geometry
exports behind an explicit full-capture action. Keep it useful for future
support logs; do not silently collect gigabytes or change the configured AA.

Offline slot inspection gives a concrete production gap: all 998337 marked
DLSS pixels contain exactly the cleared MRT6 pair `(-1.0f, 0.0f)`. There is no
foreground slot marker to match, although t33 identity fields are present.
The flat route excludes weapon motion families from source candidates and
only starts the MRT6 producer within a named world-camera run; the engine
producer independently rejects before-naming/other-camera draws. The AACF/CF
shader recipe already exists. Reuse the MRT6 target and VR's bounded original
VS position/identity history, with an explicit flat camera-domain marker;
do not mistake current slots for exact final ownership. Unknown/equal-depth
writers must update or invalidate that provenance. The existing historical
original-shader identity-fusion corpus qualifies four of five families; it
does not provide a blanket live flat SDK qualification.

General diagnostic policy implemented: ordinary configured census key
(`NumLock` by default) keeps bounded desktop shader/state discovery and normal
backend/refusal/environment reports. It does not arm manual compute probes,
cold projection audits, full input/output pixels, draw pixels/packets or the
weapon-footprint exporter. Holding Shift with the same key explicitly arms
the existing full capture. Pending full requests cannot be downgraded by an
ordinary repeat; VR's census branch is unchanged. No INI key was added and
live settings are preserved. Focused policy/wiring tests pass, including
AA-off gating and byte-identical replay of the existing 46-frame corpus.
This diagnostic-policy change does not claim to fix SDK AA engagement.

Full validation for the diagnostic policy: `build/capture-policy-full.log`,
all 122 jobs plus installer/package gates passed. Receipt fingerprint is
`edd2bced98da58d903dde81ca356530db948cf0cb5b824dcfb7225027ec78932`.

### 2026-10-05: shared animated-history implementation and SDK input qualification

Hypothesis confirmed before editing: the private DLSS input capture's 998,337
alternate-camera pixels all carry the cleared `(-1,0)` marker, while the source
adapter excludes the foreign animated producer. The missing producer and final
camera ownership prevent SDK motion/depth qualification; the configured backend
must remain unchanged.

The VR position/identity producer is extracted into a bounded shared history.
Its existing source/stencil scheduling, shaders and timer boundaries stay in the
VR adapter. Focused WARP tests pass 80,537 checks. Separately compiled original
and extracted implementations produce identical bytes for all 18 existing VR
motion maps (1,769,472 bytes). Flat may retain one four-byte GPU instance index
for its deferred ownership raster; VR asks for no new copy.

Flat-only DXBC marker variants preserve original color/depth in seven synthetic
shader families. Foreign slots use negative odd codes distinct from clear;
passing world fragments replace foreign ownership, including equal-depth draws.
The resolver now has an explicit qualified foreground-motion/depth contract,
with canonical world-convention depth. Native TAA keeps its conservative union.
Production admission remains subject to the adapter's complete qualification.

Ruled out: an invented invalid/offscreen motion vector as a shared SDK local
history reset. NVIDIA's direct NGX contract supplies a frame reset and actual
current-to-previous motion; Streamline's invalid-vector setting fills missing
camera motion and is not that contract. FSR's out-of-image history check follows
nearest-depth motion dilation, so a fabricated vector is not a guaranteed local
RGB-history exclusion. No zero-motion substitute or reset-every-frame is added.

Complete plasma packet descriptors establish that IA.VB0 is a dynamic CPU-writable
688,128-byte buffer and VS t33 is a dynamic CPU-writable 336-byte structured pool.
A bounded flat-only identity ledger can read the actual index/allocation identity
from full CPU writes and invalidate on unsupported/GPU mutation. This avoids a
per-frame GPU wait. Unique prior identities select actual captured positions;
new identities require a transition reset, and ambiguous prior inputs remain an
explicit qualification failure until equivalence is proven.

Offline qualification now covers 105,506 WARP checks for the shared history,
CPU upload witness and deferred foreground raster. The raster uses actual
captured clip positions, including signed previous W and current triangles
crossing the eye plane. Common SDK depth derives from the captured constant
clip-Z; it does not assume that CPU camera metadata is the rendered near plane.
Final slot and exact device-depth equality filter world overdraw. Unsupported
foreign discard/depth/coverage shaders remain explicit contract refusals.

The focused mono resolve suite passes with DLAA, DLSS and FSR backend stand-ins
receiving the exact foreground motion and common-near depth. A genuinely new
identity resets once; stable identities continue. Stale, missing, malformed or
unqualified maps refuse before evaluation. Native TAA keeps its previous route.
The existing mutation tests still catch all 21 first-person, 18 raster-phase and
22 steady-detail mutations. These checks establish adapter/input behavior,
not in-game SDK image quality or performance.

Independent HEAD/current mono rigs using the identical HEAD test corpus both
pass. All 20,939 complete texture readbacks are byte-identical: 16,249,856 pixel
bytes, SHA256 of the 16,584,880-byte records (including shape/format headers)
`48aa8fcc6328c4c2dfb3f3e8a7509bf28147e1a5245ec3c0be487e427744b9aa`.
This pins the existing VR/TAA/backend-input paths across the shared shader edit;
the comparison harnesses and logs are under `build/mono-shader-parity`.

Environment: the pending Epic test is Windows native D3D11, flat native HDR at
3840x2160, with the existing pinned DLSS 310.9.1 runtime. Flat history requires
the qualified 336-byte animated pool and is bounded to 64 retained draw records;
the deferred map is bounded to 16 million pixels. No VR runtime/headset selection
or eye scheduling is changed by this extraction. VR output parity is offline
evidence; it does not replace a live regression qualification on other runtimes.

The combined original-to-MRT6-to-MRT7 shader avoids competing substitutions:
one game draw writes final ownership and conservative coverage. The focused
engine rig passes 13,235 checks, including identical original color/depth bytes,
foreign/world ownership and coverage together, and null-PS depth-only markers.
Single-camera SDK admission keeps its prior route. Only an actually mixed-camera
H requires the new domain producer; private late-HDR overlay protection remains
its existing qualified clean-color boundary.

Ruled out: global upstream camera ownership proves each foreign draw's phase.
The refresh hook admits centered alternate-near cameras, but its global receipt
does not name every upload. A new bounded witness reads the actual bound b1
publication and requires the measured projection shift and near to match the
draw's phase before capture. The final focused shared-history rig passes 105,532
checks, including zero/nonzero actual GPU camera rows, incorrect phase claims,
near mismatch and unsupported off-center projections. It adds no GPU readback
to production. The runtime also invalidates history certificates on UAV writes.

Full-build qualification caught three test integration errors before installation.
The five new payloads now append after the original shader registry, preserving
all legacy byte/stage contracts; each new payload has independent registration,
compile and stage checks. The null-marker color oracle now reads exactly one
raw word per RGBA8 pixel and compares bytes across all four nonzero seeded
targets. Its former word3664 comparison exceeded the actual 960-pixel plane.
Ruled out: null-marker color corruption as that failure's cause; both the
original production policy and a temporary alternate policy pass the corrected
pixel oracle. No extra write-mask policy is retained: the D3D11 output contract
already leaves undeclared render-target outputs untouched
([D3D11 spec section 16.9.1](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)).

The HDR progress-wiring test also retained the former literal 14-slot cleanup.
The foreground path binds and clears 16 slots; other frames retain 14. Its
source-order assertion now follows that conditional count while still requiring
cleanup before the completed-preparation counter and timing step.

Final absolute-path `build.bat --jobs 4` passed all gates with exit 0, including
production DLLs, Python self-tests, GPU/CPU test rigs, PE checks and both
self-contained installer profiles. Log: `build/shared-foreground-full-final.log`.
The full-pass receipt was written at 2026-10-05 20:21:15 UTC, input fingerprint
`5d050812d44de3bb4f359ff1e9c5bde69c4c1dd28918f125bf385db8fea64091`.
Source is frozen for commit and clean-version promotion. BUILT, NOT FLOWN;
the next qualification is one plasma draw/holster transition in the configured
DLAA/DLSS/FSR mode, with ordinary NumLock if the backend refuses.

### 2026-10-05: first shared-history flight still refuses SDK AA

Verified Epic `edvr_gfx_20261005_142842.log`: `v0.18.2-9-g6ab6acff`,
build `6AC4080F`, linked 20:26:55 UTC. Sean reports weapon-drawn AA disengagement
or aliasing with DLAA/DLSS/FSR, while TAA continues. NumLock at 14:33:01 starts
the bounded general report. At 14:33:24 the foreground report has 1,384 H
attempts, zero qualified, zero foreign-seen/captured, and
`last-refusal=foreground-unknown-original-writer`. The resolve reports
`flat-resolve-foreground-contract-unqualified`. TAA has repeated 300/300
actually-treated windows at frames 54000, 54300 and 54600; the later DLSS
windows have zero completed treatment. A separate 2880x1620-to-3840x2160
interval also hits `hdr-route-needs-render-at-least-output`; the native-size
foreground refusal remains after that interval.

TAA's own kernel rejects history locally using current and prior camera-domain
masks. It displays current color in ambiguous weapon-covered pixels and still
accumulates qualified world pixels. The SDK adapter instead requires qualified
foreground geometry/history and a common depth convention for mixed-camera H;
failure refuses evaluation, and spatial recovery supplies current-frame color.
This is an adapter admission difference, not evidence that SDK AA cannot work
with weapons.

The intake is connected, but its constructor still requires the old world
family-pair admission (`d.supported=engineVelocityPoolFamilyPair`), or a known
family null prepass. Unknown pairs refuse before the new foreground counters.
For example, frame 50069/q2358 has VS AACFDCF2FB9AD809 and PS CAD1F585EDDC5641,
valid current camera/full viewport, alternate near .0675, and `supported=0`.
The old family table explicitly excludes this PS. Same-depth unknown null-PS
prepasses are also reported. The aggregate refusal does not identify the first
blocking draw; retain that limit rather than claiming one precise culprit.

Ruled out: a disconnected wrapper or wrong-depth-only latch, because H attempts
are counted only when the watched depth equals the selected H depth, and the
existing MRT6 producer still runs about 10,300 substituted draws/frame. Its
draws intentionally do not increment the new non-producer world-marker counter.
Ruled out: offline geometry parity alone proves live foreground admission,
because this verified flight never captures an admitted foreign draw.

Next: exercise the production admission planner offline with these recorded
pairs and null prepasses, distinguish original world-family support from
structural foreign geometry support, and retain bounded first-failure evidence.
Do not remove the SDK safety guard or ask for another flight before this
boundary is tested. This analysis changes no renderer or installed settings;
the installed code remains `6ab6acff`.

### 2026-10-05: admission repair and ordinary NumLock cost

Sean confirms SDK AA also appears disengaged while holstered, and ordinary
NumLock causes a sustained frame-rate dip. The same verified flight shows
discovery CPU cost near zero at 14:32:59, then about 5.2 ms/frame after the
14:33:01 general trigger. The draw wrapper rises from about 1.5 to 6.9 ms/frame.
General requests exclude bulk exporters, but still restart the old 120-second
passive observer window. The repair limits ordinary reports to three useful
frames or one second, with wall-clock expiry before the paused observer gate;
explicit full capture retains its existing longer window. No new config key.

Admission hypotheses and discriminators, before the next build:

- Old pair membership blocks structurally valid foreign geometry. Actual
  AACF/CAD1 creation bytes derive the pool ABI and produce valid foreign-only
  and combined MRT6/MRT7 pixel shaders on WARP. All 156 original executable
  instructions remain byte-identical and ordered; this is not pixel parity.
- Pre-world null prepasses need provisional world-camera proof. Actual
  84F6/ACE405 VS bytes end in B1 rows270..273 projected to o0; the generic
  classifier rejects their split coefficient temporaries. A pending receipt
  must match the subsequently named world camera/depth/extent in the same frame
  before H admission, or refuse. Shape alone does not establish the camera.
- CFCA8 uses actual CB0 rows4..7 DP4 projection. B1 phase alone is insufficient;
  the new helper checks the actual ranged/private CB0 publication, finite
  canonical near and measured raster phase. Focused WARP checks pass (105,557).
- Recorded native HDR writers EC813/B765 and 983979/350735 are projected pool
  material meshes, not fullscreen GBuffer shading. They need exact final-color
  ownership treatment, original discard/coverage semantics and actual draw
  state; a generic read-only lighting exemption would be unsupported.

Ruled out: small output files mean cheap NumLock, because the ordinary window
keeps driver/state observers running and the measured CPU cost remains high.
Ruled out: those HDR material passes merely shade the existing GBuffer, because
their actual shaders sample mesh UV material textures and lack that screen-depth
reconstruction proof. The temporary CAD1 dummy-resource harness crashes even
with the original unpatched PS; it cannot establish color parity. No flight is
requested while these production admission boundaries remain untested offline.

The structural planner caches proofs from original creation bytes and validates
the actual ranged projection publication before recording foreign motion or
provisional null ownership. The final H boundary must match every provisional
null camera/depth/extent/phase witness and the separately recorded HDR target.
Known world producers reuse checked bindings without extra per-draw CB queries.
A compact first-failure record retains frame, draw sequence, VS/PS, format,
camera, depth, stage and reason for ordinary NumLock reports.

World HDR markers share the original material PS (including discard) and the
original depth/stencil state: `engineVelocityFlatDomainBeginDraw` only changes
the marker target, derived blend and shader bindings. Conditional stencil thus
selects original color and world marker together. The new primitive/writer
tuple also certifies surviving foreign stencil coverage; legacy markers without
that provenance retain their refusal. HDR depth must write or
test EQUAL, and color must be opaque with complete RGB writes. The old packet
contains the HDR shader bytes but not their live descriptors; runtime admission
must check them, and raw shader proof alone is not flight qualification.

The full recorded-nominee audit found additional real same-DSV writers before
building: two nonnull depth-only PSs were rejected by the legacy color-output
requirement; nonpool World markers demanded an identity they do not consume;
72BDD's exact VS projection was unnecessarily tied to one companion PS. The
flat-only marker and independent VS proof paths address these without changing
legacy VR derivative behavior.

Ruled out: the old world-pair gate alone explains all SDK refusal, because
recorded foreign 7B0DC/0DF0, 8B589D/7268, 114AF/A175 and F516/B40B draws use the
same selected DSV but near .0675 versus world .025 and have real PS discard.
The existing RG32F owner stores pool identity and depth, insufficient to select
the original surviving primitive among coincident triangles or different draws
of the same pool identity. The discard guard correctly refuses this ambiguity.

The next proof uses flat-only RGBA32F ownership: retain pool/depth in xy and add
exact primitive and frame-local writer identity in zw. The original material
raster stamps both after its own discard/depth/stencil tests; H replay must match
all four fields. Triangle-list, one-instance and no-GS/HS/DS restrictions preserve
indexed invocation order through the position capture. A verified free private
PS constant slot publishes the writer identity and is restored after the draw;
missing slots, identity overflow and unsupported shape refuse. VR retains its
RG32F allocation and original derivative bytes. Required offline negatives are
coincident primitives and separate same-slot/depth draws with different motion.

The shared WARP rig now passes 114,044 checks with RGBA ownership. Its real
indexed material discards the later of two coincident same-slot/depth triangles
whose previous bone poses differ; H retains the surviving primitive's motion.
A separate same-primitive cross-draw case discards writer 2 and retains writer
1's motion. Conditional-stencil EQUAL selects the actual surviving writer;
replay with stencil disabled still recovers its motion from the tuple and
restores original stencil state/reference. Fresh world-only H, native extents
and common-near transitions also pass. The production marker rig passes 15,201
checks, including original RGB/depth bytes and exact ranged PS CB restoration.
All 14 recorded nominees prove world admission, and every pool foreign shader
creates with provenance on WARP. Generator reflection/hash/dry-run tests and all
three capture-reader Python self-tests pass.

Full absolute-path `build.bat --jobs 4` passed on 2026-10-05 at 21:57 UTC:
production DLLs, all 122 jobs, config/exports, self-contained installer payload
checks and the fresh full-build receipt. Receipt inputs are
`eaf7d1f47f90b162bd22fccdb744c2c327cac235da228544f05f3c45ab007646`.
Log: `build/flat-provenance-numlock-full.log`. No flight or hardware frame-rate
claim yet. Epic flat settings and DLSS 310.9.1 remain the test environment;
the wider owner plane applies to flat, while VR keeps its original RG32 format.
Native HDR still requires render extent at least output extent; scaled-HDR
support is a separate route, and unknown/incomplete writers still refuse with
the first-failure receipt. Ordinary NumLock expires at three useful frames or
one second; Shift+key retains explicit full capture.

### 2026-10-05: five-mode report and Astra review of c145d97d

State: FLOWN, SDK qualification still refuses. Sean reports five NumLocks: AA
off, TAA, DLAA, FSR, and DLAA with a plasma weapon drawn. The sanctioned log
reader verifies Epic log `edvr_gfx_20261005_160625.log` against installed
commit `c145d97d`: version `v0.18.2-11-gc145d97d`, build `6AC41EBB`.
Environment: flat Epic, native 3840x2160, installed DLSS 310.9.1; no VR
runtime/headset/per-eye qualification in this flight. Actual requested modes in
the log are `on`, `dlss` and `fsr`; no `off` or literal `dlaa` transition is
recorded. Treat the NVIDIA windows as Sean's DLAA-labelled tests, not proof
that that intended rendering mode ran.

Four manual general captures are recorded. Each reports the
1000-ms/3-useful-frame budget and completes at three useful frames. Four
Presents include the initial ownership boundary, not four populated frames.

| Requested test | NumLock time | Final time | Useful frames | Elapsed from key |
|---|---|---|---|---|
| TAA | 16:09:00.194 | 16:09:00.283 | 3 | 89 ms |
| NVIDIA/DLAA-labelled | 16:09:08.692 | 16:09:08.782 | 3 | 90 ms |
| FSR | 16:09:21.977 | 16:09:22.074 | 3 | 97 ms |
| NVIDIA/plasma drawn | 16:09:58.035 | 16:09:58.153 | 3 | 118 ms |

The reported AA-off press is absent; this log alone does not distinguish a
missing key event from a mode/ownership issue. Startup discovery is separate:
16:06:27.265 through 16:08:27.265, its existing 120-second budget, 887 useful
frames. Continuing five-second reports after manual completion are ordinary
runtime reporting. Manual discovery is no longer collecting for seconds, but
that does not dismiss the reported persistent slowdown. In the final windows,
`discovery=0.000 ms/frame` while the EDVR CPU receipt remains 12.54-12.72
ms/frame. This flight does not isolate the stream-output capture cost or prove
that NumLock caused the remaining production overhead. Every capture also
reports relevant-observation truncation; bounded output does not imply a
complete draw record.

TAA treats frames (1069 treated by 16:09:02.331). The holstered NVIDIA and FSR
windows then leave treated count at 1311 with zero backend calls. After a brief
additional TAA interval, the final NVIDIA count remains 1442. Final HDR
receipts have 230-238 triggers per reporting window and zero prepared/backend
calls, reason `flat-resolve-foreground-contract-unqualified`. Across the
flight: `marker-refused=0`, `identity-known=42528`,
`identity-unpublished=49649`, `certificate-missing=92084`, `H-attempts=2474`,
`H-qualified=0`. These are cumulative totals, not per-mode comparisons. The old
marker gate is passed; the SDK contract is still refused before backend
evaluation.

Ruled out: c145d97d's structural admission and primitive/writer provenance are
sufficient to engage SDK AA, because original markers now run without marker
refusal but all 2474 H attempts fail. Ruled out: the short manual NumLock
budget runs beyond three useful frames in this flight, because all four
recorded manual windows finish at three in 89-118 ms. The unrecorded fifth
press remains unresolved. Pool rotation is not established by
`identity-pool-slot-unobserved` alone.

Astra independently reviewed `9d59690d..c145d97d` and the verified flight.
Findings:

1. **P1, premature scene-depth selection.** `flat_runtime.cpp:4313-4315` binds
   `foregroundDomainDepth` to the first scene/native-sized DSV before proving
   camera, format, shader or selected H membership. Any later different
   candidate fails the whole frame. The final plasma capture supplies the
   trigger: frame 50915, q1, format 27, camera 0, VS `04873C8813207261`, PS
   `EF37E1BB2C09B26E`, DSV `000002207D606460`, reason
   `foreground-original-camera-unavailable`. Earlier scene identity refusals
   name `0000021D5FFF83A0`. The log lacks a paired selected-H DSV witness for
   that same frame, so it establishes the premature latch and failure, not the
   identity of every actual scene DSV. Keep candidate evidence by depth until
   existing scene/H selection chooses its resource; unknown writers on the
   chosen resource must still refuse. Offline regression: unrelated
   native-sized pass before a valid scene, plus an unknown writer on the
   selected scene.
2. **P1, identity acquisition never settles.** `flat_foreground_identity.h:83`
   demands the pool row after learning the actual instance index at draw time.
   `flat_animated_identity_ledger.h:93-99` makes a newly demanded slot
   unobserved and only later publication can populate it. Scene draws
   repeatedly report `identity-pool-slot-unobserved`, including holstered
   NVIDIA and FSR. Existing tests demand before publication. They do not cover
   publication-before-demand with changing slots. Discriminate ordering, slot
   rotation and the 64-row eviction limit using actual pool/instance identity,
   slot, demand/eviction event, publication epoch/sequence, and requested rows
   at publication; do not choose the repair from the refusal string alone. Keep
   authoritative identity validation.
3. **P2, capture work survives sticky refusal.** `flat_runtime.cpp:5157-5160`
   still calls foreground capture after failed identity, and
   `flat_foreground_motion.h:76` appends the captured draw before returning
   `!refusal_`. The `captured` counter therefore counts successful returns, not
   all actual GPU captures. Separate attempts/completions/cost and necessary
   next-frame history warming from work that cannot benefit either frame. This
   source path can waste work, but the current flight does not attribute its
   milliseconds.

Astra found no concrete defect in primitive/writer stamping or preservation of
original discard within the admitted draw shape. Do not replace that ownership
proof, add weapon-specific allowlists, or downgrade configured SDK AA to TAA.
Next work is offline regression and identity evidence design. No new fix,
build, install or test flight was requested as part of this review; Epic
remains on c145d97d.

### 2026-10-05: selected-depth isolation and authoritative GPU identity

State: implemented, focused regressions pass, full validation pending. The
c145d97d flight remains the last hardware evidence. No performance improvement
or SDK engagement is claimed until the new build flies.

The depth repair keeps four bounded, persistent DSV candidates. Each has its
own original-writer failures, motion history, provisional null camera
witnesses, HDR/color resources and phase/state proof. Only the exact depth
chosen by the existing H boundary qualifies. The prior selected depth is pinned
across the next frame so an early unrelated native-sized pass cannot evict its
history. A missing selected candidate or its capacity overflow still refuses;
an unknown writer on the actual selected scene still refuses. Selected H
receipts now name frame, depth pointer, candidate count/cap and
missing/overflow state. Failures on unrelated DSVs no longer poison the
selected one.

The route regression reproduces the q1 unrelated failure before scene selection
and proves the selected scene can qualify while an unknown writer on that
selected scene cannot. Audit found a separate hole:
`flatRuntimeWritten(prefix,nullptr)` does not set `prefix.uncertain`. A null
mutation before the first DSV candidate therefore needed an explicit
frame-level refusal, now checked at H and covered offline. Foreign-thread
mutation continues to set `foreignWork` and refuse at H. Per-DSV geometry
mutation handling retains existing invalidation and unknown-write rules.

The sparse CPU ledger probe reproduces `identity-pool-slot-unobserved` when a
newly encountered slot is requested after publication. It still cannot
establish whether ordering, slot rotation, row eviction or resource churn
caused the flown failures. The repair removes all of those CPU row-cache
dependencies from live SDK admission instead of guessing one. It shares VR's
existing `AnimatedVertexHistory` GPU capture: actual original VS positions,
current/previous skeleton and allocation, and an exact retained instance-index
scalar. There is no GPU readback, whole-pool CPU scan or weapon-specific
selector. The shared VR shaders and scheduling are unchanged; the new adapter
and shaders are flat-only.

CPU matching retains same-pool, same-geometry history candidates; the GPU
compares the exact slot, skeleton and allocation. A unique match uses its
actual previous positions and stored prior raster phase. Multiple matches
coalesce only when every previous triangle position is bitwise identical and
its prior phase agrees. Otherwise the map carries actual canonical current
depth with class 2, rejecting that pixel's history. It does not guess a
previous pose or veto world SDK AA. Missing current GPU identity likewise
cannot borrow prior history. Unrepresentable slots or invalid positions remain
invalid coverage. Original final-owner slot/depth/primitive/writer and
raw-depth checks remain intact. High-bit raw instance metadata is a remaining
boundary: the shared identity CS reads raw index, so those records
conservatively reject history rather than infer a masked identity.

The live path no longer queries/copies fourteen VS constant buffers and 128 VS
SRVs for a CPU input certificate on each foreign draw, and no longer tracks
sparse CPU identity uploads at every Map/Unmap. Exact GPU position agreement is
a sufficient duplicate-motion proof. The bounded ledger receipt support remains
exercised by offline tests. No config keys were added or removed. Performance
remains unmeasured on hardware; the CPU census has a separate `foreground
capture` family, and capture receipts separate attempts, GPU attempts,
completed submissions, preflight refusals and useful warming after another
frame refusal.

The original performance regression failed before the repair because an
unidentified draw still submitted an original-VS GPU capture. Preflight now
rejects unusable CPU-mode identities/cameras before SO; GPU-mode capture
instead uses the actual GPU identity witness. Valid captures continue warming
history after unrelated sticky frame failures, because the next frame can use
them. The offline test proves that preserving this warming supplies real
previous motion, not repeated reset.

Focused WARP passed 146,618 checks after shader regeneration. Added actual GPU
cases cover unavailable CPU rows/certificates, first-seen and changing slots,
allocation reuse, identical and differing duplicate poses, differing prior
phases, out-of-range GPU identity, canonical-depth rejection and exact
restoration of all fifteen touched VS SRVs and Context1 VS/PS CB0 ranges.
Existing primitive/draw discard, stencil, world-only H and CPU fixture tests
continue to pass. The SDK prep rig additionally checks DLAA, DLSS and FSR all
keep their backend engaged on class-2 ambiguity, preserve canonical depth,
reject only the affected pixel's history and avoid a whole-frame reset or TAA
substitution. Full build and clean promotion remain required.

Environment for the next test: flat Epic, native 3840x2160, DLSS 310.9.1,
preserved `edvr-flat.ini`. The per-DSV table has four entries; each original-VS
history retains its existing 64-record/32-MiB limits. VR
runtime/headset/per-eye behavior is not newly qualified. Ordinary NumLock
retains the already-proven three-useful-frame/one-second bound; startup
discovery still has its separate 120-second budget. One short holstered/plasma
comparison with configured DLAA or FSR and one NumLock is sufficient to inspect
backend engagement and selected-depth/foreground-capture receipts; no
per-weapon matrix is requested.

### 2026-10-05: Astra completion review and additional offline gates

Astra's second review found two concrete gaps before installation. Runtime
per-DSV isolation did not extend to `engine_velocity.cpp`: a global marker
depth/frame still rejected a second admitted DSV, and retrieval exposed only
one plane. A marker on A followed by a foreign/null pre-world draw on selected
B could still sticky-fail B. The old q1 camera-less flight does not prove A
stamped any marker; this is a source-proven correctness gap in the new
implementation. The new regression must exercise actual A/B GPU markers, exact
DSV lookup and ordinary world-source aliasing without clearing pre-world
evidence, rather than just the route table. Marker-layer completion is in
progress.

Ruled out: runtime candidate isolation alone completes per-DSV ownership,
because the producer/retrieval layer still had a global depth latch. Preserve
the chosen DSV's complete original-writer proof through the actual ownership
plane.

The GPU adapter also requested a whole SDK reset for `priorCount == 0`, despite
class 2 already rejecting new geometry's history locally. The new warm-world
regression failed that condition on the previous code. GPU mode now leaves
first-seen history rejection local; the legacy CPU fixture mode retains its
established initial-history reset. A real common-near convention transition
still resets once and settles on the following frame. The focused original-VS
WARP rig now passes 157,476 checks, including warm world, first-seen GPU
geometry, actual near transition, canonical depth and state restoration. Logs:
`build/foreground-local-reset-before.log` and
`build/foreground-local-reset-after.log`.

The first full build compiled both production DLLs, then failed `run_jobs.py`'s
Windows process-tree self-test. A ping grandchild survived timeout cleanup and
held its heartbeat file; this was a sandbox process-termination limitation, not
a rendering regression. The isolated self-test passes with escalated execution.
The full gate must run again under that execution context after the marker
source is frozen. No receipt, clean promotion or installation is claimed from
the failed build. No flight is requested while these offline gaps remain open.

Both Astra completion findings now have failing-before/passing-after
regressions. The marker layer uses four cached per-DSV RGBA ownership planes,
each cleared once per frame, with source-depth pins and conservative capacity
refusal. Ordinary flat source production aliases its exact cached plane; it
neither replaces nor clears earlier foreign ownership. Internal MRT6
recognition includes every cached plane so lazy source restoration cannot
mistake a previous plane for the game's own MRT6. The actual two-depth GPU test
failed `flat-domain-multiple-depths` on old code and passes after the repair: A
and B remain separately retrievable, B retains original foreign tuples through
a later named B world draw, and A's bytes are unchanged. Focused production
marker WARP rig passes 15,223 checks. Logs: `build/engine-marker-red.log` and
`build/engine-marker-green.log`. Shared VR RG32 eye/source production keeps its
profile-specific lifecycle; only flat ownership uses the new four-plane cache.

The full escalated build is rerunning against this frozen source and the
157,476-check local-reset GPU adapter regression. Hardware SDK engagement and
frame time remain unqualified until the installed build is tested.

### 2026-10-05: full validation passed, clean promotion next

The full escalated absolute-path build completed with exit 0 against the frozen
repair. Both production DLLs, the 117-job rig pool, five quiet jobs, config
contract and self-contained installer/resource checks passed.
`build/full_build_receipt.json` records `full-pass` with input fingerprint
`c0d7b0b9b34552d4e6e2b947f3ffc287cfbc0b3dce373b5a7284c40a1f16d0c1`; the log is
`build/flat-gpu-identity-depth-full.log`. This supersedes the earlier failed
process-tree gate.

Commit this same source, push main, then use the receipt-guarded DLL-only
promotion and sanctioned Epic flat installer. Preserve the live flat INI and
DLSS runtime. The next hardware check is configured DLAA or FSR with the weapon
holstered, then one plasma weapon drawn for about ten seconds each and one
lightweight NumLock. No complete flight or weapon inventory is needed. Hardware
backend engagement and frame time are still unqualified; the offline
regressions demonstrate the corrected failure paths rather than a measured
performance gain.

### 2026-10-05: 4bb67d35 flight still refuses configured SDK AA

Sean reports good TAA and apparent AA-off output with DLAA. The sanctioned log
tool verifies Epic `edvr_gfx_20261005_171647.log` as `v0.18.2-13-g4bb67d35`,
build `6AC42EC9`, linked 23:12:09 UTC. The visible SDK summaries call the
configured mode `dlss`; retain this naming distinction when interpreting the UI
report.

At 17:19:43.335 the runtime reports 2,401 H attempts, zero qualified, 112,669
actual GPU-identity submissions and zero marker refusals. The first selected-H
failure is planning `foreground-original-projection-unproven`, VS
`FC1193AFFC596F74`, PS `258B95AC99520C1F`, format 23, camera present, and exact
selected depth `000001FF7B33B160`. There are three candidates against the
four-slot cap with no overflow. Repeated samples name the same shader pair and
refusal. At 17:19:48.326 a later selected-H sample instead names
`history-budget`, VS `F516BF0201303B87`, PS `B40B0462256E31C2`, with 2,516 H
attempts and still none qualified.

Ruled out: repairing GPU identity acquisition and per-DSV marker isolation
alone restores this scene, because this verified build submits actual GPU
identity, reports no marker refusal or candidate overflow, and still qualifies
no H. This does not prove every identity match or candidate is valid. The prior
implementation defects remain independently covered by offline regressions.

Open discriminators: projection-proof parsing/classification versus genuinely
unsupported original clip construction, confirmed by the actual refused VS
bytecode and planner fields; retained history-record pressure versus legitimate
within-frame demand, confirmed by the bounded key/record lifecycle and budget
accounting. No guard relaxation, guessed motion or budget increase follows from
the refusal strings alone. Trace both paths offline before proposing another
build or asking for another capture.

Offline conclusion: the actual captured Epic VS `FC1193AFFC596F74` contains
only an input-position move to `SV_Position` and return, with no camera
constant reads. Its actual PS `258B95AC99520C1F` writes zero to target 0.
`flat_projection_recipes.h` already recognizes this exact pair as unchanged.
The SDK ownership planner instead requires a forward projection before any
ownership classification, and refuses it. Ruled out: a missing
perspective-projection parser pattern for this pair, because its actual
bytecode contains no projection. Historical per-object-motion censuses identify
a fullscreen stencil stamp, including depth-disabled cases. They do not
establish the current Epic blend/depth/stencil state; do not invent a
projection recipe or unconditionally ignore this pair.

The GPU history helper has 64 records and 32 MiB bounds and retires records
untouched for more than two frames. Thus a permanent never-retired record leak
is not established. Before a world source is named, the planner treats
structurally proven pool draws as foreign and captures them; legitimate
current/previous demand, transient geometry churn, and provisional world
captures can consume the bounded pool. Current logs omit the record/byte usage
and camera-domain demand at the refusing draw, so no budget increase or
scheduling change is justified yet.

Next diagnostic should record both missing receipts together: original bound
target masks, depth enable/write/function and stencil state/reference for the
first refused draw; and active record/byte counts plus
current/previous/provisional capture demand for history refusals. Use bounded
first-failure summaries, independent of large manual exports, and test logging
through the production failure/report path offline. The verified flight already
proves zero SDK H qualification; no second flight is needed to re-establish
that symptom. Installed graphics remain `4bb67d35`; this entry changes
documentation only.

### 2026-10-05: offline bench using the production proxy

Sean requested an offline bench before another flight. The available evidence
supports the known shader-planning and bounded history paths, not an exact
settlement replay. The four retained draw-packet captures contain 27 packets
and neither refusing shader pair. Their manifests have no build identity, and
the existing replay rejects their missing post-draw DSV payload. Do not treat
those packets as a faithful reproduction of the current failure.

The bench pins eight original shader fixtures by DXBC size, FNV identity and
SHA256 (27,312 bytes total). The latest refused
FC1193AFFC596F74/258B95AC99520C1F pair is actual captured bytecode. World and
representative alternate-camera fixtures come from prior captures. Geometry,
pose constants, render-target contents, blend/depth/stencil state and draw
ordering are reconstructed. The report preserves that distinction and
fingerprints the tested proxy DLL.

The history workload uses the production AnimatedVertexHistory on WARP. It
independently reproduces the 64-record refusal (65th distinct draw; 6,144
retained bytes), the byte refusal (eight large indexed draws; 33,551,232
retained bytes), and the fifth duplicate's occurrence cap. Reuse, transient
churn, two-frame retirement, actual buffer writes and global reset also pass.
This establishes the bounds and lifecycle; it does not establish which source
consumes the current game's budget. No budget or rendering policy was changed.

The integration process loads a staged proxy in an invisible D3D11 fixture and
enters the actual Present and draw hooks. The shipping proxy's
unsupported-host control retains its real game-build guard. A separately named
bench DLL uses the existing engine rig define and three reconstructed
emit-hook entry points; all other rendering objects are the production
objects. It tests the actual GPU slot resources, original vertex history,
ownership and H qualification, with engine job hooking explicitly outside its
evidence. A read-only, owner-thread snapshot exposes runtime planning,
original GPU identity submissions, H qualification and completed backend
counters. Exact structure size and ABI version are required. It supplies no
fabricated qualification maps or state overrides.

GPU readback also checks the actual owner plane before and after H. The scene
keeps a visible alternate-camera triangle outside the world draw; a positive
capture counter with an empty foreground cannot pass. SDK renderer PASS
requires visible world and foreign pixels at H, H qualification, and a
completed configured backend. Native TAA requires the real HDR resolve. The
unsupported-host case is a guard result, not a renderer result.

Three SDK-only cases execute the exact FC/258B shader pair with reconstructed
OM state: stencil-only with color and depth writes disabled, depth-writing,
and color-writing. Readbacks distinguish their actual effects. The guard cases
report the measured writer refusal; they do not prove that projection planning
is the right stage to reject a writer. No hash-only exemption was introduced.
The F516/B40 fixture identities are pinned but their exact plasma draw
geometry and workload are not replayed; the history tests use controlled
workloads instead.

The runner stages only under build/ and leaves game installs and settings
alone. Each scene/mode gets a fresh process. Native TAA's public INI setting
is on; the runner's readable taa label maps to it. --dry-run creates no files,
directories, subprocesses, devices or DLL loads, and the actual CLI self-test
verifies that property. Framework self-test success is separate from renderer
PASS/FAIL/UNSUPPORTED.

Run python tools/flat_sdk_bench.py --report build/flat-aa-report.json. WARP
covers deterministic D3D11 behavior; --adapter hardware uses the local adapter
for supported backend checks. Neither synthetic scenes nor WARP certify actual
settlement image quality, performance, or every VR runtime. The installed Epic
graphics remain 4bb67d35. No new flight or capture is needed to continue work
on the covered paths.

Validation: the frozen-source full build passed all gates. Its receipt input
fingerprint is
`65023684e8ab0ff28575043c0fd49a439bba33740a460705db13543ea2104584`,
independently verified against this source and compiler context. Native build
stdout is saved in `build/flat_sdk_bench_final_build.log`.

Both default 17-case matrices ran. The 64x64 flat D3D11 scene uses no VR
runtime or headset. Hardware logs identify NVIDIA GeForce RTX 5090; the
installed driver inventory reports 32.0.16.1692 and the carried DLSS DLL's
file version is `310,9,1,0`. Baseline TAA, DLAA, DLSS and FSR each complete
one real HDR resolve on hardware, with zero spatial fallback. SDK baselines
qualify one H and complete one configured backend. GPU readback at H finds 338
world pixels and 218 foreign pixels, after 338 foreign pixels before H.

Adding the stencil-only FC/258B draw changes 4,096 stencil pixels and zero
color/depth pixels. The visible owner pixels survive, but all three SDKs
refuse H with `foreground-original-projection-unproven`, with zero backend
calls and a spatial fallback. This reproduces the current first-refusal path
through the production hooks under explicit reconstructed no-write state. It
does not establish that the latest game uses that same OM state. All
unsupported-host and writer-refusal controls pass; all five history cases
pass. WARP also completes TAA and FSR, while NGX explicitly reports
unsupported GPU `0xBAD00001` after upstream qualification.

Reports are `build/flat-aa-warp-report.json` and
`build/flat-aa-hardware-report.json`, about 50 KB each. Overall renderer
verdict is deliberately FAIL because the no-write case reproduces the bug; the
framework and full-build gate being green are not a rendering fix. Next:
correct inert-draw admission using this positive and negative matrix, then
test the bounded history scheduling independently. Do not add a guessed
projection, ignore a shader pair unconditionally, increase a budget without
accounting, or request another flight to rediscover this refusal.

### 2026-10-05: inert admission corrected against the offline bench

Hypothesis: the ownership planner unnecessarily requires a projection for a
draw which cannot change color or depth. The prior hardware bench confirms the
first-refusal path: FC1193AFFC596F74/258B95AC99520C1F changes stencil only,
preserves visible world and foreign owner pixels, then refuses H before any
configured SDK backend executes. The discriminating result after correction is
a qualified H and completed configured backend with the stencil changes
preserved; actual color and depth writers must continue to refuse.

The correction uses original creation-byte proof and live output state. It has
no shader-hash exemption, invented projection or per-weapon table. The camera
classifier's Clean result is insufficient by itself: its parser also models
UAV stores. An independent cached side-effect proof rejects those stores,
atomics, unsafe declarations and unknown operations, while allowing supported
pure arithmetic and read-only operations. The original VS must be structurally
camera independent, and the original PS must be classified Clean.

Only a refusing inert candidate incurs the additional live-state queries. Lazy
substitutions are restored first. Actual shader objects and identities, DSV
and depth resource, all eight effective render-target masks, effective depth
writes including the read-only DSV flag, all supported OM UAV slots, other
graphics stages, stream output and predication are checked. A proved no-write
draw executes its original stencil operation once, without an ownership
nomination, private marker or vertex-history capture. World/pool hot paths
keep their existing queries.

An inert candidate which fails these state checks records its specific
first-failure reason under inert-state. A future live difference therefore
identifies color/depth writing, shader/DSV mismatch, another stage, UAV,
stream output or predication rather than repeating projection-unproven. No
capture payload or configuration key was added.

History-budget remains an independent open discriminator: the bench proves the
existing bounds and retirement behavior, but current game logs do not identify
the retained occupants. No budget or scheduling change follows from this
correction. The current game's exact OM state is also unmeasured; offline
success covers the tested state contract, not every settlement image or
performance condition.

Validation: all 17 hardware matrix cases PASS on the NVIDIA GeForce RTX 5090
in the 64x64 flat fixture (no VR runtime or headset). DLSS runtime file
version is `310,9,1,0`. Each stencil-only DLAA, DLSS and FSR case changes
4,096 stencil pixels and zero color/depth pixels, retains 338 world and 218
foreign pixels at H, qualifies one H, completes one configured backend, and
uses zero spatial fallback. All six color/depth writer controls and all four
host guards PASS. The five controlled GPU history-pressure cases also PASS.
WARP has 13 PASS, zero FAIL and four explicit NGX GPU-unsupported results,
with FSR's stencil-only case now PASS. These are backend execution and
state-contract results; they do not certify game image quality, frame rate or
VR behavior.

Reports: `build/flat-aa-inert-hardware-report.json` and
`build/flat-aa-inert-warp-report.json`. The frozen-source full build passed
all gates; stdout is `build/flat-inert-final-build.log`. Receipt input
fingerprint is
`d62ef1db0319d317a2c253274e6ae6f89bc9eadcc868c1478477ea47081c1eed`,
independently verified against the source and compiler context. Promotion will
use the receipt-guarded clean DLL build and preserve Epic's settings and DLSS
runtime. No additional flight was required for this correction.

### 2026-10-05: inert correction flown; history budget still blocks SDK AA

Sean reports DLAA/FSR still appear off and took NumLock. The sanctioned log
tool verifies Epic `edvr_gfx_20261005_195534.log` as `v0.18.2-16-g32db3d2d`,
linked 2026-10-06 01:52:33 UTC. The SDK runtime summary's configured string is
`dlss`; retain that distinction from the UI report. NumLock appears at
19:58:25.267 as a general capture, with 1,017 accepted and 2,500 refused
frames. These totals span mode changes; they do not demonstrate SDK backend
success.

The first-failure samples now name VS `F516BF0201303B87`, PS
`B40B0462256E31C2`, format 23, stage `history`, reason `history-budget`. At
19:58:25.267 the selected-H depth matches the refusing depth, with two
candidates against cap four and no overflow. Four selected-H samples name the
same budget refusal; a fifth sample is in TAA mode with no selected H. No
inert-state or original-projection-unproven refusal appears. At 19:58:40.539
there are 2,535 H attempts, zero qualified, 119,843 actual GPU identity
submissions, and zero marker refusals. The 1,017 treated frames in the route
report include TAA; they are not evidence that the configured SDK ran.

Ruled out: the inert admission correction alone restores SDK AA in this scene,
because this verified flight removes that first refusal but still qualifies no
H, with selected-H failures now at history allocation.

Source trace: before world naming, the domain planner classifies proven pool
draws as foreign and submits their original vertex captures immediately. Each
depth candidate owns a history with 64 records and 32 MiB limits; the budget
reason combines those two limits. Records remain for current/prior history and
retire when both parities are more than two frames old. The current per-frame
draw list also has a 64-draw bound, with a different refusal string. Existing
offline workloads prove the bounds and retirement operate as implemented; they
do not prove the scene's demand fits them.

Open hypotheses: record exhaustion from provisional or legitimate foreign
draws (record count and camera-domain occupancy discriminate); byte exhaustion
from large captured meshes (retained bytes and requested vertex count
discriminate); transient geometry churn across retained frames
(current/prior/older record counts and reuse discriminate). The log omits
these allocation fields, so it cannot select among them. Candidate overflow,
marker failure and the prior inert first refusal are not the sampled blocker.
Do not raise either budget or exempt a weapon shader from this reason alone.
Preserve the offline bench and qualify the scheduling path before requesting
another flight.

The adapter has another bound before the history allocator: 64 successful
captures in the current frame refuse the next as `foreground-draw-bound`.
Therefore a same-frame 64+1 workload cannot reproduce this flight's
`history-budget` through the adapter. A discriminating reconstructed
regression must retain prior-frame records: for example, 40 old records plus
24 new pre-world captures leave the current list below its bound but exhaust
the history's record capacity before a late foreign draw. Small meshes isolate
records from bytes. An integrated classifier/adapter test of this ordering can
expose scheduling sensitivity, but cannot establish which occupants caused the
live refusal. The 11,249 submitted captures after a sticky refusal explain
`submitted - captured`; they are next-frame warming, not unexplained failed
allocations.

Another offline discriminator is known VB/IB mutation. The history helper
invalidates matching record parities immediately but reclaims their allocation
only at the next advance. If a previous-only geometry is invalidated after
beginFrame, the adapter erases its previous draw without a current-frame
failure, while invalid records still consume capacity for later capture. This
is a provable ordering edge, not an established cause of this flight. An
offline test can distinguish it from legitimate retained history; a live
receipt would need invalid-record occupancy and mutation counts. Any shared
reclamation change must preserve active VR consumers' capture references.

### 2026-10-05: full refusal inventory and cross-frame allocation regressions

Sean clarifies that holstered and drawn weapons fail alike. Treat this as a
scene-wide qualification failure; a weapon shader at an allocation failure
does not identify every draw consuming capacity or establish a weapon-only
defect.

Correction to the preceding scoped log read: filtering for history-budget
missed another refusal in the same verified flight. The complete first-failure
inventory in `edvr_gfx_20261005_195534.log` has ten selected-H
`foreground-mixed-component-writer` samples, four selected-H `history-budget`
samples, one TAA-mode budget sample without a selected H, and 24 never-failed
samples. The mixed writer is VS `CFCA8FFC6B058630` / PS `8A08FF781272C5F6`.
Its original PS declares and writes all four target-0 components. The refusal
proves enabled blending or a partial output mask on a color-enabled target;
the existing capture does not identify the state. Before world naming, this
same pair was observed with both eventual world and alternate-camera
projections. Provisional camera classification remains an open discriminator;
no guard relaxation follows from these identities.

The expanded WARP regressions reproduce adapter allocation pressure with 40
prior-frame records plus 24 current-frame captures. The next new geometry
refuses `history-budget` while staying below the adapter's current-draw bound.
Known IB writes also reproduce a distinct allocator edge: prior captures
become unusable immediately, but their allocation still blocks a new key until
the next frame. Small meshes isolate record pressure; eight large allocations
isolate byte pressure. COM-owned snapshots survive ordinary eviction, but
outstanding captures retain a record index that can shift under compaction.
Reclamation must preserve publication through stable view identity and
distinguish never-submitted reservations from write-invalidated records.

Ruled out: plasma-only draw state explains this report, because Sean sees the
same absence of AA holstered and drawn and the full inventory contains two
different first-refusal paths. These offline regressions establish a
reclamation defect, not the live allocation occupants or the mixed writer's
blend/write state. Keep both distinctions in the build and test conclusions.

The correction reclaims explicitly write-invalidated history records only when
an allocation would otherwise exceed the existing record or byte limit, and
only when reclaiming them can satisfy that allocation. Never-submitted
reservations are distinct from invalidated records. Same-key reuse retains its
allocation; pressure reclamation retains the original shader's stream-output
program so it does not trigger an unnecessary compile. Published and pending
captures survive other-record compaction through view identity and a mutation
epoch; a stale capture cannot republish a record after invalidation and reuse.
The shared helper's 64-record, 32-MiB and vertex bounds remain unchanged.

First-failure summaries now retain bounded allocation occupancy and the
original draw's blend, sample mask, depth/stencil and color-target state. Raw
effective OM write masks remain separate from PS component masks. Typed RTV
formats are read from already-retained views on the first refusal only. The
original DSV was not returned by the existing query, so DSV flags remain
explicitly unobserved. No extra context query, GPU readback, capture export or
config key was added. The selected-H snapshot carries the same first-failure
receipt even if later draws use a different state; the bench ABI is version 2.

The renderer matrix independently exercises partial component writes and
enabled blending. Each guard case requires a real GPU color change and checks
that the first refusal's state survives a later opaque draw and H selection.
These tests preserve the safety guards; they do not establish the live mixed
writer's operation or justify bypassing it.

The first complete hardware matrix caught a bench entry-point defect: the new
guard names were absent from its CLI whitelist and never entered the scene. A
single case table now drives validation, dispatch and usage; the build-gated
self-test exercises every accepted command shape. All six guard cases then ran
through the actual proxy: partial writes changed 218 pixels, blending changed
338, and each first-failure state survived later draws and H.

Environment: flat profile, reconstructed 64x64 targets, no VR runtime or
headset. Hardware is RTX 5090, driver 32.0.16.1692, DLSS runtime file version
310.9.1.0. The 64-record shared history bound is unchanged. Captured original
shader bytecode is exact; geometry, constants, state and frame ordering are
reconstructed. This is not an exact scene replay or a visual-quality result.

Validation: the final full build passed all gates and wrote receipt
`52af2c3eff78997269bcc750ee54ba35048ac7d9951bca79da269fbcd6c0c202`, verified
against the frozen source. Console: `build/flat-history-final-build-2.log`.
Hardware: all 23 renderer/guard cases PASS and all 11 GPU history workloads
PASS (`build/flat-aa-history-hardware-final-report.json`). WARP: 19 PASS, four
NVIDIA backend cases UNSUPPORTED, zero FAIL, and all 11 history workloads PASS
(`build/flat-aa-history-warp-report.json`). Baseline SDK scenes qualify H,
invoke the configured backend and have zero spatial fallback. No TAA
substitute is introduced. An earlier concurrent-build hardware run had one
pre-draw device-creation failure `0x8876017C`; the final quiet matrix passes
that case. This does not establish the cause of that device failure or a
rendering fix for it.

Next in-game evidence is the new first-refusal receipt in ordinary periodic or
NumLock summaries: invalid/pending/current/prior/older allocation occupancy,
mutation and pressure-reclamation counts, or the mixed writer's exact blend,
write mask and world-naming state. No weapon list or long bulk capture is
required. Live AA recovery remains unqualified; holstered and drawn still
share the same unresolved scene-wide report.

## 104. Settlement SDK AA: owner marks follow the depth surface (2026-10-06)

Flight: Epic `edvr_gfx_20261006_035353.log`, verified by the log tool as
`v0.18.2-18-g4f6197e8` (build 6AC463CF, linked 02:58:23 UTC). Flat profile,
native 3840x2160, configured `dlss`, DLSS 310.9.1, RTX 5090, no VR runtime.
Sean took one NumLock at a settlement (03:58:00.873). Every settlement frame
refuses `flat-resolve-foreground-contract-unqualified` and falls back to
spatial; H-attempts reach 3635 with zero qualified. The new receipts name two
first failures, alternating between frames:

- `foreground-draw-bound` / `history-budget` on F516BF02/B40B0462, a skinned
  alpha-tested depth prepass (colour masked, stencil REPLACE ref 0): records
  64, current 57, prior 7, no invalid or mutated records; all 57 current and
  all 64 previous draws were captured before world naming.
- `foreground-mixed-component-writer` on CFCA8FFC/8A08FF78: Gbuffer RT0
  (R10G10B10A2) with ONE/ONE/MIN on colour and alpha, independent blend,
  depth write on, GREATER_EQUAL. Disassembly: a skinned VS projecting through
  CB0 rows 4..7 that passes clip W to a PS writing it to all four channels.

The same log's nominee lines already show F516BF02 and CFCA8FFC before
naming at near 0.025, the world camera's; first-person draws use 0.0675.

Offline sweep (scratch harness; production `flatDomainShaderProof` and
`flatDomainPlan` over the 10-05 F10 traces 48930, 47706 and 45214 from the
same settlement, with the dumped shader bytes). In frame 48927 the world is
named at q5359 and 146 draws reach H before it: 15 F516BF02 at 0.0675, then
44 F516BF02, 63 ACE405F4 and 3 other null-PS prepasses, 2 31869313, FC1193AF
and one CFCA8FFC, all at 0.025 with camera bytes identical to the camera named
later in the frame; then one CFCA8FFC, 15 8B589D25 and 2 7B0DC42D at 0.0675.
The rule "every pool draw before naming is foreign" captures 53-86 draws a
frame against the 64-draw bound; only 50 draws a frame use the first-person
camera. Both cameras share one b1 buffer; the first-person rows are the
world's scaled by 1.2313 with near 0.0675.

The sweep also finds what the first-failure receipt cannot show. Before H
(the frame's resolve marker follows every scene-depth draw), 128-149 more
draws refuse at planning in every frame: about 120 world-camera forward HDR
and format-60 effects as `foreground-non-Gbuffer-writer` (84 of them
359BF8FF/92FF8499 particles), 12 camera-less deferred light volumes (0357BBB2,
24DE25E4, F8FA801F, ...) as `foreground-original-camera-unavailable`, 13
in-world text draws (12 of them 0B71713B/CDDFE215) as
`foreground-original-projection-unproven`, and the first-person HDR draw
88DCF116/494506A6. Under these rules no mixed-camera frame containing a lit
point light could ever qualify; H-qualified has been zero in every flight
since the SDK domain route was built.

Ruled out: raising the 64-draw or 64-record bounds, because 48 of the 82
captures a frame are world-camera draws that need no capture, and 128-149
planning refusals would remain.
Ruled out: a weapon-specific cause at the settlement, because holstered
frames fail alike and both first failures are world-camera prepass draws.

The ownership model is changed. Deferred lighting reconstructs each pixel's
position from depth, so the owner mark must follow whatever wrote the
pixel's depth, and only that:

- Provisional world before naming. A draw whose near equals the last named
  world camera's (persisted across frames) is planned World/WorldPool with no
  capture and records the existing pending witness. H refuses unless every
  witness equals the selected world camera (all 96 camera bytes, depth,
  extent and phase), so a misprediction can only refuse, never admit. In the
  sweep 102-115 predicted draws a frame all match, and captures fall to 3-35.
- A depth-writing Gbuffer draw owns its fragments however its colour blends
  or masks (CFCA8FFC's MIN), and may follow colour writes as a depth-only draw.
  HDR writers keep the strict opaque, complete-RGB rule.
- A draw that cannot write depth never changes ownership. When the planner
  or the raster check would refuse it and its depth state cannot write depth,
  it is forwarded unchanged: no marker, capture or refusal. This covers light
  volumes, particles, decals and glows. Colour such a draw puts over another
  camera's surface reprojects with that surface's motion, the ordinary
  transparency case; the late-overlay layer still protects the known
  first-person overlays.
- Diagnostics: every distinct refusal on the selected H in the last failed
  frame (up to 8 kinds, with counts and first q) in `flat foreground refusal
  inventory` / `kind` lines, and `predicted-world`, `predicted-near` and
  `surface-preserving` counts on the SDK domain line. Bench snapshot ABI 3.

Cost seen in the same flight: in DLSS mode at the settlement the flat runtime
takes 11.9-13.7 ms of render-thread CPU a frame (foreground capture 3.4-4.8 ms
over about 10,000 slow-path validations, other 3.4-3.6 ms; present p50 16.8-
19.7 ms). TAA took about 8 ms a frame in the 10-05 flight. The change removes
about 47 stream-output captures a frame but not the per-draw validation.
Engaging the SDK at this draw count may still cost frame rate; that is a
separate optimisation, not part of this fix.

Ruled out: b1 rewrites defeating the cached world fast path, because all
10,384 supported draws on H in trace frame 48927 share one b1 write epoch.
Why about 10,000 draws a frame take the slow validation path is not measured.

Offline qualification (flat profile, reconstructed 64x64 targets, RTX 5090,
DLSS 310.9.1; shaders exact, state and workloads reconstructed; not an exact
scene replay). The admission unit tests grow from 65 to 86 checks. The bench
gains `settlement_prepass`: 72 world-camera F516BF02/B40B0462 prepass draws
before naming, plus CFCA8FFC's MIN-blend depth write in both cameras. It
requires every prepass draw predicted and none captured, depth written, no
failure anywhere in the frame, H qualified and the configured backend run.
Guards: `predicted_world_mismatch` (world near, different rows) must refuse
`foreground-pending-null-not-selected-world`; `state_blended_hdr` must refuse
`foreground-mixed-component-writer`. The former blended/partial guards now
expect admission, and colour-only draws without depth write are forwarded.
Hardware: 35/35 PASS plus GPU history pressure (`build/settlement-bench-hw-
final.json`). WARP: 21 PASS, 14 UNSUPPORTED (DLAA/DLSS need NGX), 0 FAIL,
history PASS (`build/settlement-bench-warp-final.json`). Mutation runs against scratch proxies: HEAD fails the 7 new or
changed cases; rule A removed reproduces the live `foreground-draw-bound`;
rules B or C removed, rule B widened to HDR, rule C widened to depth writers,
or the witness removed each fail their case.

Ruled out: separate staged DLL copies per bench scenario, because process
isolation only needs a separate directory and ini. Hard links replace about
2.78 GB written per matrix; nvngx_dlss.dll is staged only for DLAA/DLSS.

Compile memory, found the same day: compiling flat_runtime.cpp alone took
about 74 GB of commit and 3.5 minutes, paging the 32 GB build machine, at HEAD
as well; /Od did the same and the file's headers alone took 0.41 GB. The cause
was `FlatTraceRing traceRing{};` in State: MSVC expanded all 262,144 trace
events as an aggregate initializer once 5698007f (2026-10-04) raised the ring
to 65,536 events a frame. Every member already has a default initializer, so
plain default initialization is identical. With it the file compiles in 7.1 s
at 0.50 GB, and the full build's largest compiler process peaked at 1.37 GB
(213 s, receipt `0948558e`).

Known limits: a first-person draw at the world camera's near now refuses the
frame through its witness instead of being captured. Three cameras on one
depth refuse earlier, in the HDR selector (`source-camera-or-depth-not-
unique`), as before.

Committed as 3d5ecaf6, fast-forwarded main and pushed; origin/main confirmed.
Receipt-guarded `--dll-only` promotion passed in 52 s. Epic flat install via
`install_edvr.py` (dry run, install, `--verify-only`); the installed DLL is
`v0.18.2-19-g3d5ecaf6`. `edvr-flat.ini` (17EA3670...) and `nvngx_dlss.dll`
(3975567B...) hashes are unchanged and `edvr.ini` stays absent. Frontier was
not installed.

Next flight: the same settlement, configured DLAA, about 20 s holstered and
10 s drawn, one NumLock while there; then FSR the same way. Read against the
literal installed version: H-qualified above zero and backend calls on the SDK
domain line; `predicted-world` and `surface-preserving` counting; any
`flat foreground refusal kind:` lines (each names a remaining blocker with
its VS/PS, stage and count); and EDVR CPU a frame against 12-14 ms (104).

### 2026-10-06: 3d5ecaf6 flown; marks only first-person surfaces

Flight `edvr_gfx_20261006_061644.log`, verified against the literal installed
`v0.18.2-19-g3d5ecaf6` (build 6AC4E642). Configured DLSS from 06:16:45, FSR
from 06:19:20, at the same settlement. Sean: DLAA/FSR still did not engage;
H-qualified stays zero. The old blockers are gone. No draw-bound or
history-budget refusal; about 70 world prepass draws a frame are predicted
and every witness matches; about 3,600 draws a frame that cannot write depth
are forwarded; first-person captures are about 3-35 a frame. The inventory
names what remains, 13 kinds over the flight, with 16 windows over its
8-kind cap:

- forward HDR pool meshes (5B0068AF, 617C6E44, 33A5025C, A8E4D93B, 01A029C7,
  A6A39338; single RGBA target, alpha-tested) that write depth:
  `foreground-HDR-writes-Gbuffer`, and for two of them
  `foreground-mixed-component-writer`. The scene's HDR target is Gbuffer
  slot 3 (R11G11B10_FLOAT, format 26), so every forward HDR draw "writes the
  Gbuffer".
- 41E245D4 (world, non-pool) and 88DCF116 (first person, pool) HDR depth
  writers: `foreground-non-Gbuffer-writer` at planning.
- `foreground-color-writer-without-depth` at H: a draw with no depth bound
  writing that same texture.
- 989E0439/2A5456CF, a Gbuffer depth writer with an unproven projection
  (2 windows).

Cost: at about 10,000 substituted draws a frame the GPU frame took 31.9 ms
against 17.8 ms on 4f6197e8, and present p50 rose to about 38 ms (26 fps).
Rule B admitted about 650-770 world draws a frame into the per-draw marker
bracket (patched shader, the 4K RGBA32F owner target bound and unbound around
each draw), against about 10 a frame before. That is roughly 20 us of GPU
each.

Ruled out: the engine-bracket flush in the rule-C depth query as the main GPU
cost, because only about 150 draws a frame take that path while world
markers rose by about 700 a frame. The flush was unnecessary anyway (the
engine bracket never sets depth-stencil state) and is removed.

Change (v2, BUILT): only first-person surfaces are marked.

- World-camera draws (named camera bytes, or the predicted near before
  naming) and camera-less draws are never planned, marked, captured or
  refused. World pixels keep the engine producer's slots and the camera
  term, as single-camera SDK frames always have. The predicted witness is
  added at the draw and still has to match at H.
- First-person depth writers are captured and marked, now including HDR
  (format 26) pool meshes; colour blending and masks never refuse a depth
  writer.
- Colour bookkeeping is removed: HDR-writes-Gbuffer, colour-target-changed,
  colour-writer-without-depth and the colour branch of resource writes. Only
  writes to the depth can change which surface a pixel shows.
- The SDK prep trusts a first-person mark only while its depth equals the
  pixel's raw depth. A world surface drawn later over it takes the world
  path, whose engine lookup gives the camera term for a negative mark.

Trade-off: world objects drawn by shaders outside the engine producer's family
get camera motion in mixed frames, as they already do in single-camera SDK
frames; the 103 world domain markers had given them per-object motion.

Offline (same environment as above): the production proof admits all six
recorded first-person pairs, including the HDR mesh 88DCF116/494506A6, as
ForeignPool. Bench, hardware 44/44 PASS plus history; WARP 24 PASS, 20
UNSUPPORTED (NGX), 0 FAIL. New cases: `stale_foreign_mark` (a world
non-producer depth writer over a first-person surface leaves the stale mark;
H qualifies and resolves), `inert_depth_write_world` and
`state_blended_hdr_world` (unmarked, unrefused); `settlement_prepass` now also
requires zero world markers. The GPU prep test takes the camera term for a
first-person mark under another depth; the pre-104 rule fails 18 of its
checks and the inverted rule 48. Mutation runs: restoring the v1 world marks
passes 0 of 26 cases. Full build passed, receipt `9c3aeb0f`.

Known limit, pinned as the `state_blended_hdr` guard: a first-person HDR depth
writer other than the 88DCF116/494506A6 laser pair (or an overlay-protected
draw) still makes the HDR selector refuse the frame
(`conflicting-hdr-target-or-camera`, flat_runtime_model.h). The settlement's
only first-person HDR mesh is that pair, and its log shows H selected.

Committed 0e9f59eb, main fast-forwarded and pushed; origin/main confirmed.
`--dll-only` passed. Epic flat install: dry run, install, `--verify-only`;
installed DLL `v0.18.2-21-g0e9f59eb`, SHA256 CFCFBCAE81E8..., equal to the
build and to the install receipt. `edvr-flat.ini`, `nvngx_dlss.dll` and the
absent `edvr.ini` are unchanged. The installer's console was cut by a
`Select-Object -First` pipe after its plan lines (exit -1); the receipt and
DLL were already complete, as checked.

Next flight: the same settlement and routine (DLAA, then FSR, NumLock once in
each). Read: H-qualified and backend calls on the SDK domain line,
world-markers near zero, world-unmarked counting, any refusal kinds, and
GPU/present against 31.9/38 ms on 3d5ecaf6 and 17.8/22.5 ms on 4f6197e8.

### 2026-10-06: 0e9f59eb flown; SDK AA engages, some edges stay jagged

Flight `edvr_gfx_20261006_072437.log` on the installed `v0.18.2-21-g0e9f59eb`,
same settlement: FSR from 07:24:38, DLSS from 07:24:54, FSR again from
07:27:38. Every mixed frame qualified H (8629 of 8629 by 07:28:53); world
markers 0.0 a frame, first-person captures 0.7-36 a frame, 2,000-16,000 world
draws a frame left unmarked. SDK windows: present 11-17 ms, GPU 9.7-13.6 ms
(17.8 on 4f6197e8, 31.9 on 3d5ecaf6), EDVR CPU 7-11 ms for 6,700-10,200
substituted draws a frame.

Sean: much better, but some edges still look aliased, in DLSS and FSR alike: a
roof edge against the night sky and a blue cable spool. On the roof it is
always there, most noticeable when moving.

With an SDK backend, finishHdr shows the raw current frame instead of the
backend's result for any output pixel with a refused raster texel among its
four bilinear taps (the rule for presets that ignore the bias mask). A refused
pixel on either side of a silhouette shows an unresolved edge, and it crawls
as the camera moves. Candidates, with their signature in the refusal view and
census:

- a stale slot whose previous-depth check fails at the silhouette (a
  non-producer world surface over a producer one; v2 leaves those unmarked):
  yellow, census `stale`;
- an engine refusal: a producer record on a depth-0 sky pixel (`sentinel`),
  `masked`, `corrupt` or `unreprojectable`: white, red or magenta;
- the camera term leaving the screen (`range`, white): near the border only;
- none (the edge is painted as accepted): the backend's own result or wrong
  motion on accepted pixels, not a refusal.

Ruled out by reading the code (no flight):

- the section 102 untrusted-camera mask, because the SDK route sends the
  foreground bit (constants.debug[3] = 2) and the prep reads the mask only
  without it;
- a sky pixel with no producer slot losing its motion, because it takes the
  camera term (kind 0) with no depth check, and at depth 0 that term is the
  camera's rotation alone, which forms.

Build: the flat refusal census and view, and one temporary key.

- `advanced.temporal_aa_debug = motion_source` paints the flat prep's classes
  into H (legend in the log line when it changes) and samples the census
  while on. NumLock samples it for the next 600 resolves. The 5 s line `flat
  refusal census 5s` gives pixels, refused, stale-kept and refused by class,
  with `view=` and `local-reset=`.
- `experimental.flat_sdk_local_reset` (temporary; default `on`, the shipped
  behaviour). `off` writes no rejection for a refused world pixel that has a
  motion formed for it, so DLSS or FSR keeps its own result there and gets
  that motion. A sentinel or corrupt record forms the camera term for this
  alone. A masked or unreprojectable record is a mover's (engineBefore's own
  rule: "a masked record stays refused"), forms none and stays refused, as
  do a pixel whose camera term does not form and the first-person, reset and
  invalid-depth refusals. A red edge in the view is therefore one `off`
  leaves alone. Classes are unchanged, so the census reads the same either
  way. Hot-reloaded; each change is logged.

Validation: `flat_keep_refused_gpu_tests.h` in `tools\flat_mono_resolve_test`
runs DLAA, DLSS and FSR, with and without the foreground map, on a moved
camera. Key on: every refused probe keeps raw with no motion. Key off: a
stale slot that failed the depth check, a corrupt slot and both sentinels
(out of range, sky) carry the camera term with no rejection; off-screen
motion is kept; a masked or unreprojectable record, a Camera-class patch, a
half-float overflow, a reset frame, a depth-2 pixel and a refused
first-person pixel stay refused. Accepted pixels, and a frame with nothing
refused, are bit-identical either way; the finish shows History where no
rejection was handed; TAA never gets the bit. 13 of 13 in-rig mutants
caught. The first-person phase test's shader anchors follow the changed
motion line; restore them when the key goes. Full build green, receipt
`d9aa012f`. Bench (default key only): hardware 44/44 PASS; WARP 24 PASS, 20
UNSUPPORTED (NGX), 0 FAIL. The A/B needs the steady-detail depth check to
run (the 5 s steady line's `depth-check` ran above 0): without it a stale
slot reaches the prep as an explicit refusal and `off` leaves it.

Committed 0ed050a1, main fast-forwarded and pushed; `--dll-only` passed. Epic
flat install: dry run, install, `--verify-only`; installed
`v0.18.2-24-g0ed050a1`, SHA256 9FB04CC95CD08E73..., equal to the build.
`edvr-flat.ini` unchanged.

### 2026-10-06: 0ed050a1 flown; refusals ruled out, DLAA resolves still edges

Flight `edvr_gfx_20261006_085316.log`, verified `v0.18.2-24-g0ed050a1`: FSR
08:53:52-08:54:22, then DLAA at 3840x2160 from 08:55:24; every in-game frame
treated with history.

The view and the A/B key never worked. Neither key is in the flat profile's
allow-list (`runtimeProfileAllowsKey`, `src\common\runtime_profile.h`), and
`Config::getString` returns "off" for any key outside it. The view stayed off,
and local reset read "off" (keep the backend's result) for the whole flight,
and on main from 0ed050a1 on. Sean's toggles did nothing. The config contract
checks that a key is documented, not that the flat profile lets it through.

NumLock census (DLAA, 08:58:37 and 08:58:42, 76 and 74 sampled frames):
0.0145% of pixels refused, about 1,200 a frame of 8.3 million (sentinel about
1,020, stale about 190). Stale slots kept by the depth check: about 2.76
million a frame, a third of the screen.

Sean's DLAA and AA-off screenshots of the same roof, enlarged to the pixel:
DLAA draws the roof's thin top bevel as a continuous line where AA off breaks
it into dashes; the dome and the strut edges likewise. In a still frame DLAA
anti-aliases these edges.

Ruled out: refused pixels shown raw as the jagged edges, because the census
refuses 0.0145% of pixels, far too few to line an edge, and Sean saw the same
edges on 0e9f59eb (raw) and on this flight (backend kept).

Ruled out: settlement surfaces left unjittered, because the still DLAA frame
resolves them, which DLAA cannot do without jitter; the cameras left alone are
shadow and light (kinds 0 and 1) and one kind-4 camera refreshed 3 times a
frame.

Open: edges break up in motion, in DLAA and FSR. Candidates: (a) the
upscalers' own handling of a bright sub-pixel line in motion, which EDVR
cannot change; (b) a sub-pixel motion error in the camera term's translation
(`now[5]-old[5]` in float32: if row 275 is a large world position, walking
quantises the delta, and turning does not). Discriminator: walk past the roof,
then stand and turn so the edge crosses the screen at the same speed; compare
EDVR's TAA; log the size of row 275 and the per-frame translation.

Sean kept the A/B key for the motion question (asked 2026-10-06). Build
84f6a147: both keys join the flat allow-list (config_test pins them) and are
logged on their first read; a 5 s line `flat camera origin` gives row 275's
size, its float32 spacing and the per-frame step. Full build green, receipt
`6a6ea9d6`; `--dll-only` passed; Epic flat install verified,
`v0.18.2-27-g84f6a147`, SHA256 9D769421FE5E3A02..., `edvr-flat.ini`
unchanged (it already holds the view line, twice, and local reset `off`).

Next flight, DLAA at the roof: still; walking past; standing and turning at
the same screen speed; NumLock while walking; local reset on against off
while walking; EDVR's TAA on the same walk. Read: `flat camera origin` (a
float spacing near the walking step rules (b) in), the census in motion, and
which movement breaks the edge.

### 2026-10-06: 84f6a147 flown; inputs verified, residual is the content

Flight `edvr_gfx_20261006_092649.log`, verified `v0.18.2-27-g84f6a147`. Both
keys now read and logged: the view on at start, local reset toggled on at
09:29:06 and off at 09:29:25 while walking. DLAA (3840x2160), then TAA.

Sean: "definitely looks much better"; walking, DLAA is calmer but the roof
edge still shimmers slightly; no difference between local reset on and off.
EDVR's TAA shimmers at the roof's lower-left corner standing still.

- The view paints the roof yellow: a stale slot (an unkeyed surface over a
  keyed one) taking the camera term once last frame's depth confirms it.
  Stale-kept: 1.8 to 3.0 million pixels a frame.
- Census while walking (DLAA): refused 0.006% to 1.4% a window; the large
  windows are `range` (motion leaving the screen while turning, up to 116,000
  pixels a frame at the borders); stale refused at most about 5,600 a frame,
  sentinel about 1,000 (likely the stars: a slot on a depth-0 pixel).
- `flat camera origin`: row 275 is a local position, 1.8 to 63.5 m from its
  origin; float32 spacing at most 3.8e-6 m against walking steps of 0.03 to
  0.26 m a frame. Standing, the step is 0 or about 2e-7 m.

Ruled out: float32 quantisation of the camera term's translation, because
row 275 stays under 64 m, where the spacing (3.8e-6 m) is four orders of
magnitude below a walking step.

Ruled out: the local reset as the shimmer, because toggling it while walking
changed nothing Sean could see.

TAA standing still: the enlarged screenshot shows the roof's thin top bevel
beaded (alternate bright and dark pixels) and the corner's edge pixels
unresolved. EDVR's `taa()` checks history depth against the single nearest
texel (`oldQ`) with a 1% tolerance. At a jittered silhouette that texel is
roof one frame and sky the next, so the edge pixels lose history on
alternate frames. The steady-detail check solved the same problem with the
best of four texels; the TAA kernel never got it. A TAA-only finding: the
SDK backends do not use that check. Not changed here.

What remains with DLAA is a bright, sub-pixel, view-dependent (specular)
bevel against a black sky, shimmering slightly in motion. With jitter on
every surface, motion verified, refusals negligible and translation exact, no
EDVR input is left to correct; the backends' own handling of such a line is
the limit. Rendering above native (Elite's supersampling with DLAA on the
HDR route, which takes R >= D) is the known way to reduce it, at GPU cost.

Closed with Sean (2026-10-06): `experimental.flat_sdk_local_reset` is removed
and the raw behaviour stays: a refused pixel shows the raw current frame, as
before 0ed050a1, the behaviour flown across many scenes. The keep-refused
prep bit, its rig tests and the phase test's changed anchors are backed out.
Kept: the refusal view (`advanced.temporal_aa_debug = motion_source`, now
allowed in flat), the NumLock refusal census and the `flat camera origin` 5 s
line. The TAA single-texel depth check is offered as a separate task.
Committed 5f7e43e8 (full build green, receipt `fb1b6035`, 234 keys read and
documented); `--dll-only` passed; Epic flat install verified,
`v0.18.2-30-g5f7e43e8`, SHA256 C97FB27B83EE256A..., `edvr-flat.ini`
unchanged (its `flat_sdk_local_reset` line now names a key nothing reads).

### 2026-10-06: 5f7e43e8 flown; supersampling never runs the SDK on foot

Flight `edvr_gfx_20261006_094726.log`, `v0.18.2-30-g5f7e43e8`. Sean: the view
"was removed" and, even at SS 2.0, the roof edge still shimmers in motion.

The view was not removed. The ini had no view line at launch (first read:
off). After Sean added it at 09:52:12 the view came on, but at SS 0.75: DLSS
upscaling 2880x1620 to 3840x2160 on the copy route, and only the HDR route's
finish painted. The census ran either way.

Supersampling never ran DLSS or FSR. At SS 1.5 (5760x3240) and SS 2.0
(7680x4320) every frame was refused with
`flat-resolve-foreground-contract-unqualified`: the first-person contract
never qualified H (H-qualified 0 of about 7,140 attempts, against 100% at
native). The HDR route recovered each frame spatially (`last=spatial-fallback`,
`backend=0`), and the jitter never left its warm-up (`jitter=(0,0)`), so there
was no temporal AA at all: the SS 2.0 shimmer Sean saw is plain supersampling.
It is no evidence about DLAA, and it means supersampling above native cannot be
offered as the lever for the roof yet. OPEN: why H never qualifies above native.

At SS 0.75 the copy route ran DLSS normally (treated-jittered, about 400
accepted-history frames per 5 s).

Build (Sean asked for it): the refusal view and census on the copy route. The
copy route's frame carries the view and census flags; the resolver paints in
the copy route's compute finish (DLSS or FSR; EDVR's TAA on the copy route has
no finish), with the class texture at t11. Rig: flat_copy_refusal_view_gpu_
tests.h (DLAA, DLSS and FSR at 1x and 2x; view off bit-identical; TAA never
paints); reverting the paint line, the t11 binding or the HDR-only gate fails
32 to 40 checks. On the RGBA8 copy route the paint keeps hue, not brightness.

Root cause of the supersampled refusals (code reading; the sizes settle it):
`FlatForegroundMotion::prepareH` refused any H above 16*1024*1024 pixels
(`foreground-H-resources`). 3840x2160 is 8.3M, 5760x3240 18.7M, 7680x4320
33.2M; SS 1.25 on a 4K screen (13.0M) was under it. The reason never reached
the log: the 5 s line printed a per-frame field that every frame start
clears, so it read `last-refusal=none` with every frame refused.

Fix (Sean, 2026-10-06, with the recommendations below): the bound is 64M
pixels (`kFlatForegroundMaxPixels`; 8192x8192, 1 GB of RGBA32F map; about
530 MB at SS 2.0 on a 4K screen), and `last-refusal` is the most recent H
refusal since the previous line.

Two routes (Sean asked why; kept). H and the game's post chain run at the
render size, and only the final copy scales to the screen. An upscaler
(R < D) can only take the place of that copy; at R >= D the HDR route
resolves before bloom, DoF and tone and does not depend on the post chain's
shape (section 81). Both stay. The cost is that every feature and instrument
needs both routes; the bench gains a supersampled on-foot HDR case and a
copy-route case so every build checks both.

Next flight: same settlement, DLSS then FSR. With `temporal_aa_debug =
motion_source` under `[advanced]` in `edvr-flat.ini`, note the colour on the
jagged roof and spool edges, and NumLock once in each mode. Then drop the view,
set `flat_sdk_local_reset = off` under `[experimental]` and compare the same
edges while moving: jagged against ghosting or smearing at the silhouettes.

### 2026-10-06: TAA's history depth check takes the best of four texels

Hypothesis, from code reading (BUILT, NOT FLOWN): the shimmer EDVR's TAA
shows on the roof's top bevel and corner while standing still is `taa()`
losing the edge pixels' history on alternate frames. It kept a pixel's history
only if last frame's depth at one texel, `oldQ = int2(previous * size +
jitter.zw)`, was within 1% (floor 1e-6) of the camera term's expected depth.
A previous phase under half a texel puts that position inside the pixel's own
texel, so at a jittered silhouette the texel is the roof on one frame and the
sky on the next. The pixel then shows the raw jittered sample, bright on one
frame and black on the next: the beaded bevel in the enlarged screenshot. The
prep's steady-detail check took the best of the four texels around the
previous raster position for exactly this (section 82); `taa()` never got it.

Environment: the flat profile with `fix.temporal_aa = on` (EDVR's own TAA;
DLAA, DLSS and FSR never run `taa()`), the Epic install, Sean's 4K rig
(3840x2160 output). The check reads the resolver's own R32_FLOAT depth copy at
the render size; no fixed-size table is involved. VR is not touched: its TAA is
another kernel.

Change: `stalePreviousDepthMatches` is now `historyDepthMatches` (it has two
callers) and `taa()` asks it in place of its own single-texel lines:
`if(historyDepthMatches(previous,ExpectedDepth.Load(int3(q,0))))weight=.9;`.
Same four texels, same previous-phase arithmetic, same 1% tolerance and 1e-6
floor (`kStaleDepthRel`, `kStaleDepthFloor`; the names stay). The prep's call
is unchanged. Cost: three more R32_FLOAT loads per output pixel, on adjacent
texels.

Tests, `tools\flat_mono_resolve_test\flat_taa_history_depth_gpu_tests.h`, on
WARP through the real kernel (history a flat 128, the current frame a checker
of 64 and 192: a kept pixel reads 122 or 134, a reset one 64 or 192; last
frame's depth drawn by hand; previous phase +-0.25 per axis):
1. A jittered silhouette keeps its history: roof edges against the sky, both
   axes, both signs. The pixel whose own texel was sky but whose neighbour
   toward the phase was the roof is kept; the next one out, four sky texels,
   is reset.
2. A true disocclusion still resets: where an occluder moved away every pixel
   whose four texels held it is reset, and only the trailing-edge pixel is
   kept (the best of four's known price, and what separates four texels from
   sixteen). Depths 0.9% off either way are kept; 1.1% off either way, a sky
   and a surface twice as near are reset; at depth 1e-5, 5e-7 off is kept (the
   floor) and 2e-6 off is reset.
Both run again against the kernel with one rule flipped, through a new
test-only seam (`flatMonoResolveTestTaaBytecode`, like the prep's): 15 of 15
mutants fail the tests, and the old single-texel rule fails both.

Full build green after merging main (5f7e43e8), receipt `89e75443`: the two
tests pass on the shipped kernel. A first full build had stopped on
`heartbeat_writer_test` H5.whole (a reader-tick count under CPU load; it
passed 5 of 5 alone and in the rebuild); not touched.

What it cannot do: a thin, bright, sub-pixel specular line still has its
history clamped to a 3x3 box of the current frame's texels, and that box moves
with the jitter. That is TAA's own limit and is not changed here.

Refuted if, standing still at the roof in TAA, the bevel and corner shimmer as
before: then the depth check was not what lost the history, and the next step
is a count of the pixels `taa()` zeroes at the roof, not a threshold.

Committed as 36003718 (the shader and its tests), main merged in as 3db65989
and this entry as 2d8e6761; fast-forwarded main and pushed, origin/main
confirmed. Receipt-guarded `--dll-only` promotion passed. Epic flat install via
`install_edvr.py` (dry run, install, `--verify-only`); the installed DLL is
`v0.18.2-34-g2d8e6761`, SHA256 D5B6A47E814AE5CA..., the build's own.
`edvr-flat.ini` (8A00D126...) and `nvngx_dlss.dll` (3975567B...) are unchanged
and `edvr.ini` stays absent. Frontier was not installed. Read the flight
against that literal version: `edvr_log.py --target <the Epic game directory>
--expect-build 2d8e6761` (`epic` is not an alias) exits 2 on any other build;
today it names the 5f7e43e8 flight's log as a mismatch, as it should.

Next flight (Epic, flat profile, TAA on): the same settlement at night,
standing still, EDVR's TAA on the roof's top bevel and lower-left corner, then
DLAA on the same view. Read the edge pixels for the beading, and for any new
ghost trail where a ship or a walker leaves the frame (the price above).

### 2026-10-06: the second supersample gate; a colour for refused stale slots

The bench found a second gate behind the 16M bound. With the bound raised, H
qualified at 5760x3240, and the resolver then refused the frame with
`flat-resolve-untrusted-coverage-requires-native-HDR-TAA`. Section 102's rule
refused untrusted camera coverage (every mixed-camera frame) at any render
size other than the output, for every mode. It was written for EDVR's TAA,
whose output-domain test maps one output pixel to one render pixel. An SDK
backend with the qualified first-person map never reads that mask (the prep's
debug.w is 2), and the map, the prep and the backend all run at the render
size. The size clause now applies to TAA alone; an SDK frame still needs the
HDR route and the first-person map. Bench, a scratch proxy with only that
clause changed: all six supersampled cells (96x96 into 64x64 and 5760x3240
into 3840x2160, DLAA, DLSS and FSR) ran the backend, where the real tree
refused all six.

Ruled out: the 64M bound alone as the supersample fix, because the bench at
the real size qualified H and then hit the size clause above.

The copy-route bench case is not built: the copy route admits the game's final
copy by its exact shader hashes, which no fixture holds. A capture flight and
one to two days of scaffolding would build it. The resolver side is pinned by
flat_copy_refusal_view_gpu_tests.h; the three lines in flat_runtime.cpp that
ask for the view on the copy route are left to the flight.

The view (Sean asked, after the roof painted yellow): a stale slot the depth
check refused now paints pink, a kept one stays yellow. Before, both were
yellow and only the census told them apart.

Committed 7cc82978 on d923d5f5 and the merge of main (588a443c, which
brought 2d8e6761's TAA history depth). Full build green, receipt
`342298b1`; bench hardware 50/50 PASS (the six supersampled cells
included), WARP 25 PASS, 25 UNSUPPORTED, 0 FAIL. `--dll-only` passed. The
first install was refused ("native staging requires a proven stopped game")
while Elite was running; once it had closed, the same probe passed, and the
Epic flat install verified `v0.18.2-38-g7cc82978`, SHA256
8F958B9B574146DD..., `edvr-flat.ini` unchanged.

Next flight, on foot at the roof: SS 1.0 DLAA with the view (pink on the
edge while walking is a refused stale slot); SS 1.5 DLAA (H must qualify,
`backend` above 0 on the `flat hdr route 5s` line, no spatial fallback); SS
0.75 DLSS with the view (it now paints on the copy route); EDVR's TAA
standing still with the view off (2d8e6761's best-of-four); NumLock in each.

### 2026-10-06: 7cc82978 flown; supersampled DLAA runs; the copy route refuses the weapon

Flight `edvr_gfx_20261006_114449.log`, verified `v0.18.2-38-g7cc82978`, DLSS
mode throughout: SS 2.0 (7680x4320) from 11:45:24, SS 1.5 (5760x3240) from
11:47:54, SS 0.75 (2880x1620, the copy route) from about 11:49:50.

- Supersampling now runs the SDK on foot: H-qualified 2,982 of 2,982 by
  11:48:06, `treated-jittered-hdr`, about 250 to 300 accepted-history frames
  per 5 s at SS 2.0 and 1.5. Both gates are fixed in flight.
- Pink (refused stale): present and tiny. With the view on at SS 2.0 while
  walking, stale refused peaked at about 21,000 pixels a frame of 33 million
  (11:47:16), then about 200; the paint is luma-scaled and the scene is at
  night. Sean saw none. The roof edge is kept (yellow).
- Weapon up at SS 0.75: every frame refused, `conflicting-hdr-target-or-
  camera`, from 11:49:51; treated again with the weapon down. The copy route
  has never handled a mixed-camera frame: the first-person contract (marks,
  the map, H qualification) exists on the HDR route alone. Not a regression;
  OPEN as its own piece of work.
- The roof still shimmers in motion at SS 1.5 with DLAA running. Plain SS 2.0
  (5f7e43e8's spatial fallback, no AA) shimmered as well.

Next discriminator (no build): SS 1.0 DLAA, weapon down, walking past the
roof, with `experimental.temporal_aa_before_post` at auto (HDR route: AA in
linear HDR before bloom and tone) against off (copy route: AA after tone);
the key is read at startup. Calmer on the copy route: the HDR-space resolve
of a bright thin highlight is the cause, fixable on EDVR's side (exposure or
a tonemapped resolve). The same: the content, a highlight finer than the
samples.

### 2026-10-06: the copy-route A/B and Sean's video: the bevel follows the jitter

Flight `edvr_gfx_20261006_120258.log` (`v0.18.2-38-g7cc82978`) with
`experimental.temporal_aa_before_post = off`: the copy route at SS 1.0, DLSS as
DLAA after the game's tone, treated-jittered with history (about 300 accepted
frames per 5 s, no resets, the eight-phase jitter cycling). Sean: no
improvement; standing still, the bright dashes on the roof's top bevel pulse
rhythmically.

His 4.4 s recording (30 fps, 462x144, standing still), measured frame by frame:
the bevel runs at 1 pixel per 20.6 across. Its pixels vary by a median 28/255
frame to frame, against 1.3 on a flat patch of roof. The dash pattern slides
along the edge by up to 5 pixels between frames, which is the line moving
about a quarter pixel up and down. The profile's autocorrelation has a small
bump at four video frames (133 ms), one eight-phase jitter cycle at 60 fps.
The output keeps part of the jitter on this line: DLSS does not fully settle
a highlight about a pixel wide.

EDVR's input is right. The shift in the game's camera rows is measured every
frame against the phase DLSS is told, converted as `2*px/width` (y negated),
and the log shows no mismatch (max error about 2e-7).

Ruled out: the HDR-space resolve (a bright highlight resolved before the tone
map) as the cause, because the copy route, which resolves after the tone,
pulses the same.

Ruled out: a jitter scale or sign error, because the measured row shift
matches the phase handed to DLSS every frame.

What remains is the upscalers' handling of a bright highlight about a pixel
wide on a slope, and the jitter is what moves it. Levers left, none an EDVR
input fix: another DLSS model, more supersampling, or a longer jitter cycle (a
slower, smaller pulse; section 84 removed the phase-count switch after the VR
hills showed no change).

### 2026-10-06: HDR-route weapons, the jitter cycle key, the witness line

Flight `edvr_gfx_20261006_124106.log` (`v0.18.2-38-g7cc82978`, before_post back
to auto, HDR route at SS 1.0). Sean: AA turns off for plasma weapons and any
weapon that can aim down sights, when aiming. The log: in every refused
stretch (12:43:38-58, 12:45:39-54, 12:48:14-29) H qualification failed with
`foreground-pending-null-not-selected-world` on about two frames in three. A
pre-naming world witness, one of two, did not match the selected world camera
at the tone pass. The resolver refused (`foreground-contract-unqualified`) and
the frame went to spatial fallback. `conflicting-hdr-target-or-camera` was
rare (one frame in a window). The earlier session at 12:29 (weapon up at SS 1.0)
ran with `temporal_aa_before_post = off` still set, so it was the copy route.

Built and installed (`v0.18.2-43-g52565eb8`, on main through 00d27f59, full
build receipts `af2686ad` and `329274ee`, then `091596e9` after merging
main):
- `advanced.temporal_aa_jitter_phases` (8 to 64, default 8; upstream camera
  route only, since the legacy lighting patch refuses a nudge past 7/16 px;
  live; restarts history; `phases=` on the 5 s jitter line).
- The witness line: each pending-null witness keeps its draw (VS, PS, kind),
  and the first 12 refusals log which witness differs, how (depth, size,
  phase, camera), both cameras' scale, near and position, and the first
  differing float.

Flight `edvr_gfx_20261006_125940.log`: 16 phases from 13:02:09, 32 from
13:02:37, both logged. Sean: no difference; the roof line still pulses. No
aiming down sights, so no witness lines yet.

Ruled out: the jitter cycle's length as the lever on the roof's pulse,
because 16 and 32 phases looked the same as 8. A pulse locked to the cycle
would have slowed from about 7.5 to 1.9 per second; the edge changes every
frame instead.

Left: the other DLSS models (L, M, J), and a thin-highlight filter before
the upscaler (Sean's suggestion: find one-pixel bright ridges and spread
them, at a small look cost).
