# Shared temporal AA for VR and flat Elite

## Status

- **State:** architecture proposal, 2026-09-23, based on source at `77d5be36`.
  No rendering or installer implementation in this change.
- **Recommendation:** two installer artifacts, one graphics implementation, one
  temporal pipeline, separate VR and mono frame adapters. Flat installs enable
  only temporal AA and its required support services.
- **Open:** flat camera/projection ownership, scene/depth identity, resolve
  boundary, UI ordering, render-scale ownership, and mod hook ordering need a
  measured desktop capture. Working VR inputs do not prove these paths.
- **Ruled-out pointer:** the kinematic arc's Status records rejected motion
  estimates and the nonexistent engine velocity buffer. Reuse engine-record
  motion; do not revive estimation or the retired deferred UI replay.
- **Next session:** one instrumented flat capture with AA initially off,
  collecting all signatures in section 7. No headset session needed for
  discovery; existing VR behavior still requires regression qualification.
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
