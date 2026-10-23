# Game binary map

## Status

- **What this is:** an index of the game code and data EDVR binds to, by
  system: what each item is, which source file binds it, and which doc
  explains it. It holds no addresses, so a game update does not make it
  stale. Terms are defined in `GLOSSARY.md`.
- **Where the addresses are:** a build-bound address is the constant in the
  source file named here; the arc doc of each retarget records the move
  (`docs\build-334473.md`, `docs\build-334925.md`, with the build map in
  `.agents\build-<build>\map_all.json`). Doc-only addresses are not carried by
  a retarget; "Doc-only addresses" below says how to find each again.
- **Verified:** on 332841, REA (Ghidra 12.1.4) placed every bound code address
  in an analyzed function and matched 16 hooks' entry bytes. On 334473, an
  independent REA and masked-byte placement agreed with the build map on 171
  of 173 addresses (`docs\build-334473.md`, "Independent cross-check").
- **Open:** (1) what the kind-5 and kind-3 branches in "Projection kind" do,
  which is what a 5->3 switch changes for native on-foot stereo
  (`docs\stereo-cpu-sharing-2026-10-16.md`); (2) whether `openvr_api.dll` is
  delay-loaded (`docs\architecture.md` says so; Ghidra lists plain imports and
  the PE delay-import directory was not read).
- **Next:** decompile those branches; add a re-find feature for each
  doc-only address that still lacks one.

## Map by system

Source files are under `src\`; docs under `docs\`. "doc-only" means no source
binds it (see the next section).

### Loader (Oculus route)

| Name | What | Source | Doc |
|---|---|---|---|
| kLoadLibraryIatRva | KERNEL32 LoadLibraryW import slot | common\elite_oculus_profile.h | openxr-oculus-selection-2026-09-14 |
| kLoaderCallRva, kLoaderReturnRva | the `call [import]` in the SDK shim and its return | common\elite_oculus_profile.h | same |
| route profile fingerprints | FNV pins over the selector and SDK functions | common\elite_oculus_profile.h | same, build-334473 |

### Cameras, Explorer Cam, avatar, joints

| Name | What | Source | Doc |
|---|---|---|---|
| kFreeCameraRva | FreeCameraActivity per-frame update | d3d11\explorer_cam_core.h | design-explorer-cam-free-camera-2026-10-07 |
| kCollisionRva, kBoxPushRva | camera sweep collision; push out of the commander box | d3d11\explorer_cam_core.h | same |
| kCameraUiRva, kZoomDofRva | VanityCameraUIActivity and its DOF/zoom controls | d3d11\explorer_cam_core.h | same |
| kControllerRva | VesselCameraMountControl update (vtable only) | d3d11\explorer_cam_core.h | same |
| kAvatarFadeRva, kFadeModeRva, kFadeAmountRva | AvatarModelComponent dither fade and its two globals | d3d11\explorer_cam_core.h | same |
| kFindJointRva, kFindSite1/2Rva | skeleton `FindJoint(name)` and two call sites | d3d11\explorer_cam_core.h | same |
| kPartNameTableRva, kPovNameRva | part-name table; `def_c_povCamera_joint` | d3d11\explorer_cam_core.h | same |
| kHead/FootL/FootRNameRva, kRrVtableRva, kAoVtableRva | joint-name strings; RuntimeRigComponent and AvatarObject vtables | d3d11\explorer_cam_follow_core.h | same |

### VR camera job, flat camera, transition flash

| Name | What | Source | Doc |
|---|---|---|---|
| per-frame VR job | runs the camera driver, then the coordinator | doc-only | design-transition-flash-engine-fix-2026-09-23, canted-projection |
| kConsumerRva | camera driver (4x4 mailbox copy) | d3d11\transition_flash_eye_base.cpp | design-transition-flash-engine-fix-2026-09-23 |
| composer, camera setter | build both eye cameras ("Projection kind") | doc-only | canted-projection, stereo-cpu-sharing-2026-10-16 |
| coordinator, view loop | upload the headset camera block; per-view render update | doc-only | stereo-cpu-sharing-2026-10-16, design-flat-camera-integration |
| kRefreshRva | flat camera refresh (mid-function hook) | d3d11\flat_camera_inject.cpp | design-flat-camera-integration |
| kUploadRva | camera block upload | d3d11\flat_camera_producer_probe.cpp | design-flat-temporal-aa-2026-09-23 |
| kRendererRva | builds the mono camera | d3d11\flat_stationary_camera_capture.cpp | terrain-culling |
| producer handshake | the render thread's wait | doc-only | stereo-cpu-sharing-2026-10-16 |

### Frustum, visibility, motion, skinning

| Name | What | Source | Doc |
|---|---|---|---|
| kFrustumRva, kVisGlobalRva | frustum test; visibility global byte | d3d11\cull_gate_probe.h | design-occlusion-culling-2026-09-22 |
| kPartTestRva, kPartCallReturnRva | per-part frustum and LOD test; its return in the draw-item builder | d3d11\kinematic_eval_hook.cpp | same |
| kBucketBuildRva, kEvalRva, kRigEvalRva | draw-item builder; kinematic and rig eval | d3d11\kinematic_eval_probe.h | engine-render-performance-2026-09-19, settlement-flicker-2026-09-17 |
| kLookupRva, kAppendRva | velocity lookup; append site (mid-function) | d3d11\engine_velocity.cpp | kinematic-motion-injection-2026-09-19 |
| kStoreRva | store hook | d3d11\store_hook.cpp | design-occlusion-culling-2026-09-22 |
| kOwnershipOpcodeWindowRva, writer call sites | ownership opcode window; object-record writer calls | d3d11\object_record_writer_probe.cpp | kinematic-motion-injection-2026-09-19 |
| kSkinJobsRva | skinning job assembly | d3d11\skin_entity_hook.cpp | kinematic-motion-injection-2026-09-19 |

### Light grid

| Name | What | Source | Doc |
|---|---|---|---|
| kBuilderRva, kRefreshRva, kCopyRva | cluster builder; view-row refresh; view-block copy | d3d11\light_grid_cache_core.h | light-grid-cache-2026-10-13 |
| override byte, tile px, slices, near, far, kForceRebuildRva | globals the builder reads; memo override | d3d11\light_grid_cache_core.h | same |

### Terrain cull

| Name | What | Source | Doc |
|---|---|---|---|
| kTargetRva, kCallRva, kSiteRva | cull-plane builder; its one call; the call-site pin | d3d11\terrain_cull_cover_core.h | terrain-culling |
| per-view record writer, planet draw-list builder | write one record per view; the caller | comment in d3d11\terrain_cull_cover_core.h | terrain-culling |

### UI supersampling, window, SSAA hold

| Name | What | Source | Doc |
|---|---|---|---|
| kUiSsSetter/GetterSlotRva, kUiSsSetter/GetterRva | Supersampling setter and getter vtable slots and targets | d3d11\ui_sizing_math.h | ui-layer-2026-09-23, vr-supersampling-gate-2026-10-10 |
| UI size chain, UI panel sites and functions, aspect/1080/1920 | return addresses the UI verdict reads; panel scale | d3d11\ui_sizing_math.h | ui-layer-2026-09-23 |
| kLoaderSlotRva, kWrapperRva, kLoaderRva | fxcfg loader slot, its wrapper, LODDistanceScale store | d3d11\vr_ssaa_hold.cpp | vr-supersampling-gate-2026-10-10, design-settlement-lod-bias-2026-09-22 |
| kRequestRva, kCopyRva | window-mode request; settings copy | d3d11\vr_window_trampolines.cpp | design-unified-install-2026-10-12 |

### Vertex flush, docking-state readers, present

| Name | What | Source | Doc |
|---|---|---|---|
| kFlushIaRva | FlushIA (input-assembler flush) | d3d11\vertex_resync_core.h | scanner-body |
| pad-state readers | four docking-state readers | d3d11\pad_state_core.h | optimized-station-2026-10-12 |
| present | no game address: EDVR hooks the D3D11 swap chain | d3d11\device_hook.cpp | architecture |

## Doc-only addresses

A retarget moves only what `src\` binds. Find these again on a new build with
REA (`open_binary`, keep the session) by the feature given:

| Item | Find it by |
|---|---|
| camera setter | the only `C7 84 24 74 02 00 00 05 00 00 00` in `.text` (`search_bytes`); unchanged in 332841 and 334473 |
| composer | the setter's only caller (`procedure_callers`) |
| per-frame VR job, coordinator | the composer's call chain; the coordinator has one caller, inside the VR job |
| projection-kind readers | `cmp dword [reg+0x244], imm8` with reg = helper base camera+0x20: `search_bytes "83 B8 44 02 00 00"` with mask `FF F8 FF FF FF FF` |
| view loop, producer handshake | from the coordinator (`procedure_callees`); handshake via stereo-cpu-sharing's profile |
| planet draw-list builder | the per-view record writer's caller |
| `f3dDevice::PrePresent` | the string (`search_strings`) |

## Game features

Facts about the executable that have held across builds (observed on 332841
and 334473):

- **Imports:** D3D11CreateDevice and CreateDXGIFactory1; `OPENVR_API.DLL`
  (VR_InitInternal, VR_GetGenericInterface, VR_ShutdownInternal,
  VR_GetInitToken, VR_IsInterfaceVersionValid). No OpenXR and no
  d3dcompiler import.
- **OpenVR interfaces:** IVRSystem_012, IVRCompositor_014, IVRChaperone_003,
  IVROverlay_011, IVRExtendedDisplay_001.
- **LibOVR** is not imported: it is loaded by name (`LibOVRRT%hs_%d.dll`)
  through LoadLibraryW, with an `ovr_*` name table.
- **VR settings strings:** HMDRenderTargetMultiplier, StereoscopicMode,
  IPDAmount, StereoFocalDistance, CycleRenderScale, HMDService,
  HMDHiddenAreaMesh, StereoExtraSwapChain, Stereoscopic_FrameCombineNode.

## Projection kind

The projection kind (values in `GLOSSARY.md`) is the camera struct field at
**camera+0x264** (`kCamKind`, `d3d11\flat_camera_inject.cpp`). The setter and
the readers below address it as **+0x244 from a helper base, camera+0x20**
(`design-flat-camera-integration`, "Cache-helper addendum"); camera+0x244 itself
is inside the cached view-projection matrix, not the selector. Known from logs
and earlier RE: the eyes are kind 5, the on-foot world camera kind 3
(`design-flat-temporal-aa-2026-09-23`). Added by REA (332841, rechecked on
334473):

- The **composer** gets each eye's pose (HMD service at `this+0x7D0`, vtable
  `+0xB0`) and projection (`+0xB8`, near/far), then calls the setter for eye 0
  and eye 1. With byte `this+0x660` set it skips the HMD and reuses poses and
  projections cached at `this+0x670..0x768`.
- The **setter** builds the block at the helper base (camera+0x20). Relative
  to that base it writes the basis (+0x00), position (+0x30), flags (+0x230),
  near/far (+0x234/+0x238), the HMD `+0x90` outputs (+0x240/+0x260), the kind
  (+0x244, camera+0x264, always 5) and the projection (+0x2B0).
- Five instructions compare the kind through the helper base: one to 0, one to
  2, one to 3, and two to 5 (one of them in the mono-camera builder,
  kRendererRva).

A 5->3 switch (inferred) moves the eyes from the custom headset projection to
the trigonometric one: out of the two kind-5 branches and into the kind-3
branch, which sits beside the light grid's view-row refresh.

## Re-deriving after a patch

1. **Archived executable:** `install_edvr.py` archives the installed exe on
   every real install; `python tools\archive_game_exe.py --target steam`
   does it by hand. Without the old build there is nothing to map from.
2. **Retarget:** follow `docs\build-334925.md`, "For the next retarget": the
   build map over every build-bound address (`--rvas-file`, not `--seed`,
   which covers only the render-pipeline targets), then the retarget and pin
   scripts in `.agents\build-<build>\`.
3. **REA** for what the build map reports CHANGED, AMBIGUOUS or NOT-FOUND, and
   for the doc-only addresses. A cold Ghidra import of the game took 39 min
   and about 2.5 GB of Java heap; set `GHIDRA_HEADLESS_MAXMEM=16G` and keep one
   session open rather than re-importing per question.

## Not in this doc

Raw REA output and decompiled game code stay local, outside the repo. The repo
is public: record names, struct offsets, byte features and short descriptions.
