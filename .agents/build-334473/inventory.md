# Build 334473 retarget inventory (generated 2026-10-20; machine-readable: inventory.json)

Entries: 397 (pe_identity 24, code_rva 90, data_rva 25, expected_bytes 63, struct_offset 60, build_string 41, build_list 7, test_pin 87). A co-declared table/array is one entry, so distinct constants are more (about 200 distinct code/data RVAs).

**PE old -> new** (analysis/ copies): stamp 0x6A989634 -> 0x6ACD513D; SizeOfImage 0x6409000 -> 0x640C000; 7 sections and headers 0x400 unchanged; .text VS 0x4DB1E9E -> 0x4DB4AEE (ends 0x4DB5AEE); .rdata 0x4DB3000 -> 0x4DB6000; .data 0x5E63000 -> 0x5E66000; .pdata 0x6054000 -> 0x6057000. SHA-256 e6be8bbe...4e988 -> 6891f70a...857e93. The PE pair is declared 11 times (see JSON pe_identity); all bump together.

## Per feature (counts = pe/code/data/bytes/struct)

| Feature | P/C/D/B/S | Runtime validation | Build-independent fallback |
|---|---|---|---|
| Oculus selector bypass (elite_oculus_profile.h; route; installer SHA pin probe.cpp:365) | 3/28/4/21/0 | PE+section/hdr check, 11 FNV fingerprints, call/IAT/vtable chain; all-or-nothing, route not installed on mismatch (log only) | no |
| Terrain cull cover (terrain_cull_cover_core.h) | 2/3/0/2/1 | stamp+size path, else .text signature scan (1 call site + callee head + 0.5f) | YES: scan finds 1 site in 334473 (see below) |
| Vertex resync (vertex_resync_core.h) | 2/1/0/1/6 | stamp+size, 16-byte FlushIA prologue | no |
| Light grid cache (light_grid_cache_core.h) | 2/3/6/3/5 | stamp+size, builder prologue; refresh/copy-ctor 16-byte pins | no |
| Explorer Cam (explorer_cam_core.h, _follow_core.h) | 2/11/6/8/11 | stamp+size, per-hook prologue, name-literal + part-table + vtable-slot checks | no |
| Kinematic eval hook set (kinematic_eval_hook/probe) | 1/16/0/8/6 | stamp+size, 0x430EFE0 prologue; primary/pool/merge/clear/part-test have own bytes+FNV | no |
| Engine-record velocity (engine_velocity*) | 0/2/0/1/2 | verifyEngine: stamp+size, 32+23 byte compares; rides the kinematic set | no |
| Cull gate probe (cull_gate_probe.h, object_probe.cpp) | 0/1/1/1/2 | part-test 15 frame rows; frustum prologue; vis global unchecked | no |
| Scheduler stack probe | 2/1/0/1/0 | stamp+size, 14-byte prologue x2; kRangeHi filter | no |
| Object record-writer probe (+ object_classification) | 1/5/0/2/2 | stamp+size, 16-byte lookup prologue, 5-byte call sites | no |
| Skin join entity hook (skin_entity_*) | 0/1/0/1/1 | stamp+size (ecm pair), 28-byte prologue | partial: job-table prefix join |
| Admission store hook + optimized station | 0/1/0/1/2 | stamp+size (ecm pair), 16-byte prologue | no |
| Pad-state probe (pad_state_*) | 0/1/0/1/1 | stamp+size (ecm pair), 6-byte pin x4 | no |
| UI panel sizing, fix.ui_quality (ui_sizing_math.h, ui_panel_scale.cpp) | 1/4/2/4/3 | stamp+size, both 30-byte sites, RIP targets, prologues, float values | no (kUiPanelShape scan only counts) |
| VR Supersampling gate/hold, window trampolines (vr_ssaa_*, vr_window_trampolines) | 2/4/1/3/6 | stamp+size (kUiPanelStamp), loader/copy prologues; offsets unchecked | no |
| Transition-flash eye base | 1/1/0/1/1 | stamp+size, prologue at 0x28431D0 | no |
| Flat camera injector / VR census (flat_camera_inject.cpp, vr_camera_census_core.h) | 0/2/0/1/5 | 16-byte prologue ONLY, no PE gate | no |
| Flat camera producer probe (default off) | 0/1/0/1/1 | 16-byte prologue ONLY | no |
| Flat stationary capture (flat_stationary_camera_*) | 2/4/5/1/4 | stamp+size, 15-byte prologue, vtable compares | no (fails closed) |
Also: vscreen_res.cpp (panel width) is a pure shape scan, no RVA or PE gate: **already build-independent**. Text-only 332841 mentions: orbital_width*, temporal_pass.cpp, ui_depth.cpp, device_hook.cpp, edvr.ini:379, build.bat. Log-only exe+RVA reporters, not bound: format_support_log, game_call_probe, vr_camera_census (callerRva), openxr. proxy.cpp:35 kVerifiedBuilds={"330683"} is stale (advisory, vscreen_res log wording). Shader hashes excluded (6 entries dropped as not exe-bound).

## Already works on 334473 (fallback)

- **Terrain cover**: offline scan of analysis/EliteDangerous64-334473.exe finds exactly one site: fragment A 0x4458D5D, call 0x4458D87, callee 0x4441580 (head matches, mulss reads 0.5f at 0x4DE28B8). Old: 0x4455FDD / 0x4456007 / 0x443E800. Optional to retarget. Its rig and edvr_log.py/terrain_cover_fixture.log pin old text (below).
- vscreen_res.cpp: shape scan (see above). Everything else refuses on the PE pair (fail-closed), and the 334473 exe cannot pass until the pair and RVAs change together.

## Ungated / weakly gated (retarget priority)

- kinematic_eval_hook.cpp:123,422-423 + kinematic_eval_probe.h:33,38,45: after the pair is bumped, kJobRvas[6], 0x431AFE0, 0x42B4420, 0x4312E00, 0x369C9C0 are patched on identity + the 0x430EFE0 prologue + CodeHook decode only; the rig-veto emulation CALLS base+0x434DD50/0x42B5760 and writes [coll+0x629] with unchecked callees (default on in VR). Stale RVAs here would be patched.
- flat_camera_inject.cpp:99-107 (+flat_camera_stubs.h:66): 16-byte prologue compare at 0x592200 and NO PE gate; writes camera+0x250/0x28C/0x290 and rewrites the stack return slot (frame contract R0=0x88 unverified).
- explorer_cam_core.h:44-46 kFadeModeRva 0x5E9DC28 (WRITTEN 0, restored at unload) / kFadeAmountRva 0x601E088: identity-gated only (explorer_cam.cpp:1795); .data moved, a wrong RVA writes game memory.
- store_hook.cpp:26 + optimized_station.cpp:190 + flat_world_store.h:77: writes rec+0x208/+0x234 behind identity + 16-byte prologue only (no body hash); ctx+0x1A940/views +0x40/0x6A0 layout unchecked.
- vr_window_trampolines.cpp:30,178,272 + vr_ssaa_hold.cpp:33,34,413: writes ctx+0x3564 (via a+0x1180), loader obj +0x13C; reads settings obj +0x30 (drives the write); no byte compare covers them; the window install has no identity check of its own (relies on vr_ssaa_gate.cpp:126); kCtxSsOff duplicates kUiSsOffCur.
- pad_state_core.h:22: 6-byte pin (push rbx; sub rsp,20h) shared by 4 readers; weak for a mis-aimed RVA. object_record_writer_probe.cpp:261-276: register roles/stack offsets unverified beyond call-site bytes.
- cull_gate_probe.h:119 / object_probe.cpp:245: kVisGlobalRva 0x5EA3399 byte read with no gate (SEH, read-only); object_probe.cpp:242 frustum prologue check has no identity gate. flat_camera_producer_probe.cpp:49: prologue only (default off).
- light_grid_cache_core.h:66-83: .data globals 0x5E70F7E/80/90/94/98, 0x5F27612 read via volatile reads after identity + builder prologue only (gated, but each moves with .data).
- scheduler_stack_probe.h:49 kRangeHi 0x4DB3000 = old .text end; new end 0x4DB5AEE (0x4DB6000 .rdata start): a capture filter, silently wrong if not bumped. FNV hashes (kinematic_eval_hook.cpp:1003-1096, oculus profile) cover rel32/RIP operands, so they change on ANY layout shift.
- Installer: src/installer/probe.cpp:365 SHA pin makes plan.cpp:556-580 block 334473; elite_oculus_profile.h:44 and tools/elite_oculus.py:25 pin the same SHA.

## Test pins under tools/ (file: pinned lines; update with the src symbol)

camera_census_fixture.log 117; vr_camera_census_test/vr_camera_census_test.cpp 84; elite_oculus.py 53; explorer_cam_test/explorer_cam_test.cpp 49; edvr_log.py 44; object_classification_test/object_classification_test.cpp 31; flat_stationary_camera_test/flat_stationary_camera_test.cpp 23; terrain_cover_test/terrain_cover_test.cpp 21; ui_quality_test/ui_quality_test.cpp 20; c2_coexist_test/c2_coexist_test.cpp 19; vr_camera_census_glue_test/vr_camera_census_glue_test.cpp 18; elite_oculus_test/elite_oculus_test.cpp 16; vtable_test/vtable_test.cpp 16; flat_camera_vr_test/flat_camera_vr_test.cpp 13; build_diff.py 8; vertex_resync_test/vertex_resync_test.cpp 7; object_classification.py 6; light_grid_cache_test/light_grid_cache_test.cpp 4; cull_gate_probe.py 3; cull_gate_probe_test/cull_gate_probe_test.cpp 3; engine_velocity_test/emit_tests.h 2; install_edvr.py 2; optimized_station_test/optimized_station_test.cpp 2; pad_state_test/pad_state_test.cpp 2; skin_entity_hook_test/skin_entity_hook_test.cpp 2; terrain_cover_fixture.log 2; vertex_resync_test/mutants.py 2; vr_world_route_gpu_test/vr_world_route_gpu_test.cpp 2; build_diff_targets.json 1; engine_velocity_test/engine_velocity_test.cpp 1; engine_velocity_test/primary_copy_observer_tests.h 1; native_temporal_test/native_temporal_test.cpp 1; openxr_system_test/abi_caller.cpp 1; skin_entity_hook_test/mutants.py 1; ui_quality_test/ui_panel_budget_test.h 1; vscreen_fit_fixture.log 1; vscreen_fit_test/vscreen_fit_test.cpp 1

Notes: elite_oculus.py also checks bytes 0x4E8256/0x4E86BB that the C++ profile lacks and mirrors the SHA and 11 FNVs. build_diff.py TARGET_SEED (16 RVAs) and build_diff_targets.json are the old-build baseline for the mapping tool: `diff` against 334473 reports every target UNRELIABLE (layout moved), so re-run capture/map. camera_census_fixture.log and the vr_camera_census/c2/flat_camera_vr rigs use kFlatCameraKnownCallers (0x594E13/0x594EAB/0x594FE1/0x58DE73) mostly as sample data. Rigs with literal struct offsets: explorer_cam_test, flat_stationary_camera_test, engine_velocity_test/emit_tests.h, vertex_resync_test, cull_gate_probe_test. cull_gate_probe.py:927 SIZE_FACTOR re-reads two .rdata floats.
